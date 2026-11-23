// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <swofty/db.hpp>
#include <swofty/events.hpp>
#include <swofty/live.hpp>
#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>
#include <swofty/overlay.hpp>
#include <swofty/overlay_ui.hpp>
#include <swofty/storage.hpp>

SWOFTY_SETTING(std::string_view, menu_key, "F7");
SWOFTY_SETTING(bool, show_menu_prompt, true);

namespace {

// --- Struct & Flow Offsets (invariant across 1.0.141.7865 & 1.0.145) ---
constexpr uintptr_t MANAGER_MODES          = 0x0c0;
constexpr uintptr_t MANAGER_ENTRANTS       = 0x290;
constexpr uintptr_t ES_LISTS               = 0x560;
constexpr uintptr_t ENTITY_ENTRANTS        = 0x2e0;
constexpr uintptr_t ENTRANT_SIZE           = 0x088;
constexpr uintptr_t ENTITY_LIST            = 0xe18;
constexpr uintptr_t ENTITY_MODE            = 0xf08;
constexpr uintptr_t ENTITY_TRACKS          = 0xf58;
constexpr uintptr_t ENTITY_REGISTRY        = 0x1030;

constexpr size_t WK_SESSIONS               = 0x220;
constexpr size_t WK_CURRENT                = 0x238;
constexpr size_t WK_TRACK                  = 0x278;
constexpr size_t SESSION_ID                = 0x008;

constexpr size_t STATE_HASH                = 0x0b0;
constexpr size_t STATE_STATUS              = 0x0b8;
constexpr size_t STATE_CHILDREN            = 0x0c0;
constexpr size_t STATE_CHILD               = 0xbd8;

void ilog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    swofty::log("%s", buf);

    static std::mutex log_mtx;
    static std::FILE *fp = nullptr;
    std::lock_guard<std::mutex> lk(log_mtx);
    if (!fp) {
        char path[MAX_PATH]{};
        if (GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH)) {
            std::string dir_path = std::string(path) + "\\swofty";
            CreateDirectoryA(dir_path.c_str(), nullptr);
            std::string file_path = dir_path + "\\instant_race.log";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            bool big = GetFileAttributesExA(file_path.c_str(), GetFileExInfoStandard, &fad) &&
                       (fad.nFileSizeHigh != 0 || fad.nFileSizeLow > 512u * 1024u);
            fp = std::fopen(file_path.c_str(), big ? "w" : "a");
        }
    }
    if (fp) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(fp, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        std::fflush(fp);
    }
}
constexpr size_t LOAD_SYNC                 = 0xcd0;
constexpr size_t FRONTEND_SCENES           = 0xd20;
constexpr size_t SCENES_MAIN               = 0x040;
constexpr size_t MAIN_SPAWNED              = 0x1048;

constexpr uint32_t STATE_APP               = 0xe11edb87;
constexpr uint32_t STATE_FRONTEND_WORLD    = 0x290b11ae;
constexpr uint32_t STATE_FRONTEND          = 0x957880c6;
// The menu's exit latches. Instant Race leaves the menu by the game's exit interrupt (eInterruptExit), which normally
// means quitting the game, so the game never resets what it latches on the way out. Both are cleared at each launch:
// - StateFrontendWorld +0xdc0: non-zero makes StateFrontendWorld::onStateUpdate return eLinkExit, so every later menu
//   exit (career, Grand Prix, another Instant Race) unwound the whole game flow (black screen).
// - StateFrontend +0xf90 bit 0x08: StateFrontend::onStateInterrupt requests the "ClosingGame" fade-out only while it is
//   clear, so a second Instant Race waited forever for a fade that never came.
constexpr size_t   FW_EXIT_FLAG            = 0xdc0;
constexpr size_t   FE_INTERRUPT_FLAGS      = 0xf90;
constexpr uint8_t  FE_CLOSING_GAME         = 0x08;
constexpr uint32_t STATE_LOAD_SESSION      = 0x17e3cd0a;
constexpr uint32_t STATE_SESSION           = 0x1505f42a;
constexpr uint32_t FADE_FRONTEND           = 1u << 1;

// --- Dynamically Resolved Game Addresses ---
struct GameAddrs {
    bool scanned = false;
    uintptr_t flow_link = 0;
    uintptr_t game_manager = 0;
    uintptr_t set_mode = 0;
    uintptr_t mode_by_predicate = 0;
    uintptr_t gp_predicate = 0;
    uintptr_t set_setup_mode = 0;
    uintptr_t rounds_clear = 0;
    uintptr_t add_track = 0;
    uintptr_t build_season = 0;
    uintptr_t fill_entrants = 0;
    uintptr_t weekend_structure = 0;
    uintptr_t weekend_service = 0;
    uintptr_t rounds_component = 0;
    uintptr_t message_bus = 0;
    uintptr_t post_begin = 0;
    uintptr_t post_end = 0;
    uintptr_t vt_msg_begin = 0;
    uintptr_t vt_msg_end = 0;
    uintptr_t game_root = 0;
    uintptr_t despawn_main_scene = 0;
} g_addrs;

// --- Game addresses -----------------------------------------------------------------------------------------------
// Every game address comes from a Swofty Launcher binding (Addon Browser rule: addons use the launcher's bindings,
// no own byte patterns, fixed addresses or code patches). The bindings were added to the launcher for Instant Race
// (SwoftyLauncher PRs #1467, #1514, #1515); an older launcher lacks some of them and Instant Race then stays off.
struct BindingSlot {
    uintptr_t *slot;
    const char *name;
};

std::string g_missing_bindings;

uintptr_t bind_name(const char *name) {
    return (swofty::api && swofty::api->resolve) ? swofty::api->resolve(name) : 0;
}

void resolve_all_game_addrs() {
    if (g_addrs.scanned) return;
    if (!swofty::api || !swofty::api->resolve || !swofty::api->module_base()) return;

    const BindingSlot slots[] = {
        {&g_addrs.flow_link, "site_FeFlow_AfterStateLink"},
        {&g_addrs.game_manager, "g_Game_Manager"},
        {&g_addrs.set_mode, "Game_Game_SetCurrentGameMode"},
        {&g_addrs.mode_by_predicate, "Game_GameModes_FindByPredicate"},
        {&g_addrs.gp_predicate, "Game_GameMode_IsGrandPrix"},
        {&g_addrs.set_setup_mode, "Game_GameModeEntity_SetSetupMode"},
        {&g_addrs.rounds_clear, "Vm_Node_Clear"},
        {&g_addrs.add_track, "Game_ChampionshipRounds_AddTrack_Thunk"},
        {&g_addrs.build_season, "Game_CustomSeason_RebuildFromRounds"},
        {&g_addrs.fill_entrants, "Ent_Fill_EntrantsFromList"},
        {&g_addrs.weekend_structure, "Sess_Build_DevWeekendStructure"},
        {&g_addrs.weekend_service, "k_Sess_WeekendSetupComponentDesc"},
        {&g_addrs.rounds_component, "k_Game_CustomTrackListComponentDesc"},
        {&g_addrs.message_bus, "g_Game_MessageBus"},
        {&g_addrs.post_begin, "Game_MessageBus_PostEntrantChangeBegin"},
        {&g_addrs.post_end, "Game_MessageBus_PostEntrantChangeEnd"},
        {&g_addrs.vt_msg_begin, "vt_Game_EntrantsDocumentOpenedMsg"},
        {&g_addrs.vt_msg_end, "vt_Game_EntrantsDocumentClosedMsg"},
        {&g_addrs.game_root, "g_Game_Root"},
        {&g_addrs.despawn_main_scene, "Game_FrontendSceneManager_DespawnMainScene"},
    };
    std::string missing;
    for (const BindingSlot &b : slots) {
        if (!*b.slot) *b.slot = bind_name(b.name);
        if (!*b.slot) missing += std::string(missing.empty() ? "" : ", ") + b.name;
    }
    g_missing_bindings = missing;
    if (missing.empty()) {
        g_addrs.scanned = true;
        ilog("[instant-race] All %zu game addresses come from launcher bindings", sizeof(slots) / sizeof(slots[0]));
        ilog("[instant-race] flow=%p mgr=%p set_mode=%p pred=%p setup=%p rounds_clr=%p add_trk=%p bld_ssn=%p fill=%p",
             (void *)g_addrs.flow_link, (void *)g_addrs.game_manager, (void *)g_addrs.set_mode, (void *)g_addrs.gp_predicate,
             (void *)g_addrs.set_setup_mode, (void *)g_addrs.rounds_clear, (void *)g_addrs.add_track,
             (void *)g_addrs.build_season, (void *)g_addrs.fill_entrants);
        ilog("[instant-race] more: wk_struct=%p wk_service=%p rounds_comp=%p msg_bus=%p post_begin=%p post_end=%p vt_begin=%p vt_end=%p game_root=%p despawn=%p",
             (void *)g_addrs.weekend_structure, (void *)g_addrs.weekend_service, (void *)g_addrs.rounds_component,
             (void *)g_addrs.message_bus, (void *)g_addrs.post_begin, (void *)g_addrs.post_end,
             (void *)g_addrs.vt_msg_begin, (void *)g_addrs.vt_msg_end, (void *)g_addrs.game_root,
             (void *)g_addrs.despawn_main_scene);
    } else {
        static std::string said;
        if (said != missing) {
            said = missing;
            ilog("[instant-race] This Swofty Launcher does not provide: %s. Update the launcher to use Instant Race.", missing.c_str());
        }
    }
}

enum class LaunchStage : int {
    idle = 0,
    set_mode = 1,
    wait_rounds = 2,
    leave_frontend = 4,
    loading_session = 5,
    done = 6
};

struct EraInfo {
    std::string id;
    std::string label;
    int64_t row_index = -1;
};

struct SeatChoice {
    std::string team_id;
    std::string team_label;
    std::string driver_id;
    std::string driver_label;
};

struct TrackInfo {
    std::string id;
    std::string label;
};

struct RaceConfig {
    std::string era_id = "f1_2025";
    std::string era_label = "F1 25";
    int64_t era_row_index = -1;
    std::string team_id = "";
    std::string team_label = "Random Team";
    std::string driver_id = "";
    std::string driver_label = "Random Driver";
    std::string track_id = "silverstone";
    std::string track_label = "Silverstone";
    std::string weather = "Clear";
    int time_of_day = 0; // index into kTimesOfDay (0 = the track's default)
    float laps = 5.0f;
    int distance = 0; // index into kDistances: 0 = the lap slider, else a share of the track's real race distance
    float ai_difficulty = 85.0f;
    bool surprise = false; // hide era/car/track/weather and roll them at Start
};

// Race start time. preset = the game's start-time preset in WeatherSessionSettings (-1 = leave the game's own choice).
// GameWeatherSystem::setupSession takes, with +0xb10 and +0xb11 both clear, the start preset of each dataset from
// +0xb28 + d*8 and of the session being set up from +0xb19, and turns it into a time from the track's solar times:
// 1 = the track's dawn time, 2 = morning, 3 = midday, 4 = afternoon, 5 = the track's sunset time, 6 = an hour later,
// 7 = midnight. Midnight only renders at tracks with night lighting (track weather config +0x29d set: Bahrain, Jeddah,
// Singapore, Las Vegas, Qatar, Abu Dhabi); elsewhere the game shows sunset, so Night falls back to Sunset there.
// Preset 6 is left out: at a day track it lands on the real sunset and looks the same as preset 5.
struct TimeOfDayChoice {
    const char *label;
    int preset;
};
constexpr TimeOfDayChoice kTimesOfDay[] = {
    {"Track Default", -1}, {"Sunrise", 1}, {"Morning", 2}, {"Midday", 3},
    {"Afternoon", 4},      {"Sunset", 5},  {"Night", 7},
};
constexpr int kTimesOfDayCount = int(sizeof(kTimesOfDay) / sizeof(kTimesOfDay[0]));
const char *time_of_day_label(int i) { return kTimesOfDay[(i >= 0 && i < kTimesOfDayCount) ? i : 0].label; }

// "Dry to Wet" / "Wet to Dry" are parked: rebuilding the datasets after the game's build gives a white screen
// (ROADMAP "Changing weather"). Add them back here once the custom-slot route works.
// Fixed weather for the whole race. The index is the game's weather type (0 clear .. 5 storm).
constexpr const char *kWeathers[] = {"Clear", "Light Cloud", "Overcast", "Light Rain", "Heavy Rain", "Storm"};
constexpr int kWeathersCount = int(sizeof(kWeathers) / sizeof(kWeathers[0]));
const char *next_weather(const std::string &w) {
    for (int i = 0; i < kWeathersCount; ++i)
        if (w == kWeathers[i]) return kWeathers[(i + 1) % kWeathersCount];
    return kWeathers[0];
}
uint8_t weather_type(const std::string &w) {
    for (int i = 0; i < kWeathersCount; ++i)
        if (w == kWeathers[i]) return uint8_t(i);
    return w == "Wet" ? 4 : 0;
}

std::mutex g_mutex;
RaceConfig g_cfg;
bool g_menu_open = false;
bool g_was_focused = false;
bool g_initialized_roll = false;
std::atomic<bool> g_in_session{false};
std::atomic<uint64_t> g_frontend_seen_ms{0};
swofty::overlay::ui::State g_ui_state;

std::atomic<int> g_stage{static_cast<int>(LaunchStage::idle)};
uint64_t g_stage_ts = 0;
uint64_t g_load_exit_ts = 0;
bool g_load_sync_done = false;
int g_load_sync_tries = 0;
std::atomic<bool> g_chose_sess{false};
std::atomic<int> g_track_stat{-1};
void choose_race_session();
bool g_fade_released = false;
bool g_scene_despawned = false;
std::atomic<uint8_t *> g_flow_root{nullptr};
bool g_flow_hook_installed = false;
bool g_raced_once = false; // an Instant Race has run in this game session (log only)
// Grid diagnostics (log only): per launch, how often the game built session settings, whether the weekend was laid
// out again, and where the cars stand once the session is live.
std::atomic<int> g_launch_no{0};
std::atomic<int> g_bss_calls{0}, g_bss_race{0}, g_bss_idle_logged{0};
std::atomic<bool> g_relaid{false};
int g_grid_snaps = 0;

std::mt19937 g_rng{static_cast<unsigned>(GetTickCount64())};

using ModeByPredicateFn = uint8_t *(void *modes, void *const *predicate);
using SetModeFn = void(void *modes, void *mode);
using BuildSettingsFn = uint64_t(void *mode, uint8_t *settings);

BuildSettingsFn *orig_build_settings = nullptr;
// The lap count the game's own build gave the race (its Grand Prix race length setting), before ours replaced it.
std::atomic<int> g_game_laps{0};
std::atomic<int> g_fuel_logs{0};
using WeatherSetupFn = void(void *ws, uint8_t *settings, void *config);
WeatherSetupFn *orig_weather_setup = nullptr;

template <class T> bool rd(const void *p, size_t off, T &out) {
    if (!p) return false;
    const auto *a = static_cast<const uint8_t *>(p) + off;
    if (!swofty::mem::readable(a, sizeof(T))) return false;
    std::memcpy(&out, a, sizeof(T));
    return true;
}

bool cursor_int(const swofty::db::Cursor *cur, std::string_view field, int64_t &out) {
    swofty::db::Value v{};
    if (swofty::db::cell(cur, field, &v) != swofty::db::Result::ok) return false;
    if (v.tag == SWOFTY_DB_INT || v.tag == SWOFTY_DB_ID || v.tag == SWOFTY_DB_ENUM) {
        out = v.i;
        return true;
    }
    return false;
}

std::string prettify_token(std::string s) {
    if (s.starts_with("sw_")) s = s.substr(3);
    if (s.starts_with("f1cg_")) s = s.substr(5);
    if (s.ends_with("_vehicle_package")) s = s.substr(0, s.size() - 16);
    if (s.ends_with("_gp")) s = s.substr(0, s.size() - 3);
    for (char &c : s) {
        if (c == '_') c = ' ';
    }
    bool cap = true;
    for (char &c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            cap = true;
        } else if (cap) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            cap = false;
        }
    }
    if (s == "Brhgp") return "Brands Hatch GP";
    if (s == "Imsgp") return "Indianapolis Motor Speedway";
    if (s == "Lgsgp") return "Laguna Seca";
    if (s == "Lbcgp") return "Long Beach";
    if (s == "Lisgp") return "Portimao (Portuguese GP)";
    if (s == "Mgcgp") return "Magny-Cours (French GP)";
    if (s == "Bsagp") return "Buenos Aires";
    if (s == "Gsygp") return "Channel Islands GP";
    if (s == "Laxgp") return "Los Angeles Street Circuit";
    if (s == "Nycgp") return "Manhattan Street Circuit";
    if (s == "Wawgp") return "Warsaw Street Circuit";
    if (s == "Cncgp") return "Cancun Street Circuit";
    if (s == "Lemans") return "Circuit de la Sarthe (Le Mans)";
    return s;
}

[[maybe_unused]] bool classify_era(const std::string &id, std::string &out_label) {
    if (id == "f1_2025") {
        out_label = "F1 25";
        return true;
    }
    if (id == "f1_2026") {
        out_label = "F1 26";
        return true;
    }
    if (id.find("zenith") != std::string::npos || id.find("equal") != std::string::npos ||
        id.find("career") != std::string::npos || id.find("story") != std::string::npos ||
        id.find("my_team") != std::string::npos || id.find("ghost") != std::string::npos ||
        id.find("tutorial") != std::string::npos || id.find("pro") != std::string::npos ||
        id.find("world") != std::string::npos || id.find("beta") != std::string::npos ||
        id.find("safety") != std::string::npos || id.find("f2_") != std::string::npos ||
        id.find("_tr_") != std::string::npos) {
        return false;
    }
    for (int yr = 2013; yr <= 2024; ++yr) {
        std::string ystr = std::to_string(yr);
        if (id == "sw_" + ystr || id == "f1_" + ystr || id == "f1cg_f1_" + ystr || id == "swofty_" + ystr) {
            out_label = ystr + " Swofty";
            return true;
        }
    }
    return false;
}

std::vector<EraInfo> get_installed_eras() {
    std::vector<EraInfo> out;
    bool has_2026 = false;

    HMODULE core = GetModuleHandleW(L"swofty_core.dll");
    if (core) {
        using ContentOpsFn = const SwoftyContentOps *(*)();
        auto fn = reinterpret_cast<ContentOpsFn>(GetProcAddress(core, "SwoftyCoreContentOps"));
        if (fn && fn() && fn()->owned && fn()->owned("f1_2026") == SWOFTY_OWNERSHIP_OWNED) {
            has_2026 = true;
        }
    }

    // A season counts as installed when a season pack's grid list (<prefix>_f1_<year>, e.g. sw_f1_2016) is in the
    // database; every season addon adds one, so new seasons and other season families show up without a mod update.
    // f1cg_f1_<year> lists are only display copies of the sw_ ones (the game does not load them), so they are not
    // seasons of their own.
    std::map<std::string, int64_t> rows;
    struct Found { int year; std::string prefix; std::string id; };
    std::vector<Found> found;
    if (swofty::db::available()) {
        swofty::db::Cursor cur{};
        int64_t row_idx = 0;
        if (swofty::db::open("entrants.entrant_lists", &cur) == swofty::db::Result::ok) {
            do {
                if (const char *name = swofty::db::row_name(&cur)) {
                    std::string id(name);
                    if (!rows.count(id)) rows[id] = row_idx;
                    size_t f1 = id.find("_f1_");
                    bool shaped = f1 != std::string::npos && f1 > 0 && f1 <= 12 && id.size() == f1 + 8;
                    for (size_t i = 0; shaped && i < f1; ++i)
                        shaped = std::islower(static_cast<unsigned char>(id[i])) || std::isdigit(static_cast<unsigned char>(id[i]));
                    for (size_t i = f1 + 4; shaped && i < id.size(); ++i)
                        shaped = std::isdigit(static_cast<unsigned char>(id[i])) != 0;
                    if (shaped) {
                        std::string prefix = id.substr(0, f1);
                        int yr = std::atoi(id.c_str() + f1 + 4);
                        bool known = std::any_of(found.begin(), found.end(), [&](const Found &x) { return x.id == id; });
                        // swp_f1_<year> lists sit beside Swofty's seasons but hold current cars (seen: "Haas 26" in
                        // swp_f1_2013), so they are not seasons of their own.
                        if (prefix != "f1cg" && prefix != "swp" && yr >= 1950 && yr <= 2100 && !known) found.push_back({yr, prefix, id});
                    }
                }
                ++row_idx;
            } while (swofty::db::step(&cur) == swofty::db::Result::ok);
        }
        // Every season pack offered on the Grand Prix picker adds its grid list to Swofty's shared setup mode
        // swofty_f1 (selectable_entrant_lists, a comma-separated field), whatever the list is called. Lists named
        // like *_tt are Time Trial-only copies; the year is the first 4-digit number in the name.
        // Find the swofty_f1 row: by label, else by walking the table; then read the field whatever its tag.
        std::string sel;
        std::string why;
        {
            swofty::db::Cursor mode{};
            bool at = swofty::db::row("entrants.entrant_setup_modes", "swofty_f1", &mode) == swofty::db::Result::ok;
            if (!at) {
                swofty::db::Cursor walk{};
                if (swofty::db::open("entrants.entrant_setup_modes", &walk) == swofty::db::Result::ok) {
                    do {
                        const char *nm = swofty::db::row_name(&walk);
                        if (nm && std::string(nm) == "swofty_f1") { mode = walk; at = true; break; }
                    } while (swofty::db::step(&walk) == swofty::db::Result::ok);
                } else {
                    why = "entrants.entrant_setup_modes not readable";
                }
            }
            if (at) {
                // The field by cell, then by its full key (entrant_setup_mode_<row>.<field>); entrant_lists as a
                // fallback (every season pack appends to both).
                std::string base = "entrants.entrant_setup_modes.entrant_setup_mode_" + std::to_string(mode.index) + ".";
                for (const char *field : {"selectable_entrant_lists", "entrant_lists"}) {
                    swofty::db::Value v{};
                    swofty::db::Result r = swofty::db::cell(&mode, field, &v);
                    if (!(r == swofty::db::Result::ok && v.s && *v.s)) {
                        swofty::db::Value v2{};
                        swofty::db::Result r2 = swofty::db::get(base + field, &v2);
                        char b2[160];
                        std::snprintf(b2, sizeof b2, "%s%s: cell %s (tag %u), key %s%s %s (tag %u)", why.empty() ? "" : "; ",
                                      field, swofty::db::name(r), unsigned(v.tag), base.c_str(), field, swofty::db::name(r2),
                                      unsigned(v2.tag));
                        why += b2;
                        if (r2 == swofty::db::Result::ok && v2.s && *v2.s) { v = v2; r = r2; }
                    }
                    if (r == swofty::db::Result::ok && v.s && *v.s) { sel = v.s; break; }
                }
            } else if (why.empty()) {
                why = "no swofty_f1 row";
            }
        }
        {
            static bool listed = false;
            if (!listed) {
                listed = true;
                std::string names;
                for (const auto &kv : rows) names += (names.empty() ? "" : ", ") + kv.first;
                ilog("[instant-race] [era4] entrant lists in the database (%zu): %s", rows.size(), names.c_str());
            }
        }
        static std::string last_why;
        if (!why.empty() && why != last_why) {
            last_why = why;
            ilog("[instant-race] Grand Prix picker season lists %s: %s", sel.empty() ? "not read" : "read on retry", why.c_str());
        }
        if (!sel.empty()) {
            {
                std::string all(sel), id;
                static std::string last_all;
                if (all != last_all) {
                    last_all = all;
                    ilog("[instant-race] Grand Prix picker season lists (swofty_f1): %s", all.c_str());
                }
                for (size_t i = 0; i <= all.size(); ++i) {
                    char c = i < all.size() ? all[i] : ',';
                    if (c != ',' && c != ' ') { id += c; continue; }
                    if (!id.empty() && rows.count(id) && !(id.size() > 3 && id.compare(id.size() - 3, 3, "_tt") == 0) &&
                        id.rfind("f1cg_", 0) != 0 &&
                        !std::any_of(found.begin(), found.end(), [&](const Found &x) { return x.id == id; })) {
                        int yr = 0;
                        for (size_t k = 0; k + 4 <= id.size() && !yr; ++k) {
                            bool four = true;
                            for (size_t j = 0; j < 4 && four; ++j) four = std::isdigit(static_cast<unsigned char>(id[k + j])) != 0;
                            bool edge = (k == 0 || !std::isdigit(static_cast<unsigned char>(id[k - 1]))) &&
                                        (k + 4 == id.size() || !std::isdigit(static_cast<unsigned char>(id[k + 4])));
                            if (four && edge) yr = std::atoi(id.substr(k, 4).c_str());
                        }
                        if (yr >= 1950 && yr <= 2100) found.push_back({yr, id.substr(0, id.find('_')), id});
                    }
                    id.clear();
                }
            }
        }
    }
    // Catch-all: any other grid list with a year in its name that is not one of the game's own lists (those start
    // with f1_, f2_, f3_, career_ or classic) is an addon season too, whatever its author calls it. Time Trial copies
    // (*_tt) and the f1cg display copies are skipped.
    for (const auto &kv : rows) {
        const std::string &id = kv.first;
        if (std::any_of(found.begin(), found.end(), [&](const Found &x) { return x.id == id; })) continue;
        bool stock = false;
        for (const char *p : {"f1_", "f2_", "f3_", "career_", "classic", "f1cg_", "swp_"})
            if (id.rfind(p, 0) == 0) stock = true;
        if (stock || (id.size() > 3 && id.compare(id.size() - 3, 3, "_tt") == 0)) continue;
        int yr = 0;
        for (size_t k = 0; k + 4 <= id.size() && !yr; ++k) {
            bool four = true;
            for (size_t j = 0; j < 4 && four; ++j) four = std::isdigit(static_cast<unsigned char>(id[k + j])) != 0;
            bool edge = (k == 0 || !std::isdigit(static_cast<unsigned char>(id[k - 1]))) &&
                        (k + 4 == id.size() || !std::isdigit(static_cast<unsigned char>(id[k + 4])));
            if (four && edge) yr = std::atoi(id.substr(k, 4).c_str());
        }
        std::string prefix = id.substr(0, id.find('_'));
        bool same = std::any_of(found.begin(), found.end(), [&](const Found &x) { return x.year == yr && x.prefix == prefix; });
        if (yr >= 1950 && yr <= 2100 && !same) found.push_back({yr, prefix, id});
    }
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.year != b.year) return a.year < b.year;
        if ((a.prefix == "sw") != (b.prefix == "sw")) return a.prefix == "sw";
        return a.prefix < b.prefix;
    });
    for (const Found &f : found) {
        std::string ys = std::to_string(f.year);
        if (f.prefix == "sw") {
            // Prefer the f1cg list for the menu (nicer team/driver names); the build maps it to sw_f1_<year>.
            std::string id = rows.count("f1cg_f1_" + ys) ? "f1cg_f1_" + ys : f.id;
            out.push_back({id, ys + " Swofty", rows[id]});
        } else {
            std::string tag = f.prefix;
            for (char &c : tag) c = char(std::toupper(static_cast<unsigned char>(c)));
            out.push_back({f.id, ys + " (" + tag + ")", rows[f.id]});
        }
    }
    if (rows.count("f1_2025")) out.push_back({"f1_2025", "F1 25", rows["f1_2025"]});
    if (has_2026 && rows.count("f1_2026")) out.push_back({"f1_2026", "F1 26", rows["f1_2026"]});

    if (out.empty()) {
        out.push_back({"f1_2025", "F1 25", -1});
        if (has_2026) out.push_back({"f1_2026", "F1 26", -1});
    }
    static std::string last_logged;
    std::string ids;
    for (const EraInfo &e : out) ids += (ids.empty() ? "" : ", ") + e.id;
    if (ids != last_logged) {
        last_logged = ids;
        ilog("[instant-race] Seasons found: %zu (%s)", out.size(), ids.c_str());
    }
    return out;
}

std::vector<std::string> get_gp_mounted_track_ids() {
    std::vector<std::string> mounted;
    resolve_all_game_addrs();
    if (!g_addrs.game_manager || !g_addrs.mode_by_predicate || !g_addrs.gp_predicate) return mounted;
    void *mgr = nullptr;
    if (!rd(reinterpret_cast<void *>(g_addrs.game_manager), 0, mgr) || !mgr) return mounted;
    void *modes = nullptr;
    if (!rd(mgr, MANAGER_MODES, modes) || !modes) return mounted;
    void *pred = reinterpret_cast<void *>(g_addrs.gp_predicate);
    uint8_t *e = reinterpret_cast<ModeByPredicateFn *>(g_addrs.mode_by_predicate)(modes, &pred);
    if (!e || !swofty::mem::readable(e, ENTITY_TRACKS + 8)) return mounted;
    uint8_t *tracks = *reinterpret_cast<uint8_t **>(e + ENTITY_TRACKS);
    if (!tracks || !swofty::mem::readable(tracks, 0x38)) return mounted;
    for (auto *q = *reinterpret_cast<uint8_t **>(tracks + 0x28), *qend = *reinterpret_cast<uint8_t **>(tracks + 0x30);
         q && q < qend && swofty::mem::readable(q, 16); q += 16) {
        auto *rec = *reinterpret_cast<uint8_t **>(q + 8);
        const char *name = rec && swofty::mem::readable(rec, 0x80) ? *reinterpret_cast<const char **>(rec + 0x78) : nullptr;
        if (name && *name) {
            mounted.emplace_back(name);
        }
    }
    return mounted;
}

std::vector<TrackInfo> get_installed_tracks() {
    std::vector<TrackInfo> out;
    auto mounted = get_gp_mounted_track_ids();
    auto is_mounted = [&](const std::string &id) {
        if (mounted.empty()) return true;
        for (const auto &m : mounted) {
            if (m == id) return true;
        }
        return false;
    };
    auto add_track = [&](const std::string &id) {
        if (id.empty() || id.find("test") != std::string::npos || id.find("showroom") != std::string::npos ||
            id.find("frontend") != std::string::npos || id.find("generic") != std::string::npos) {
            return;
        }
        // A spare slot the Fuji Speedway add-on maps into; Swofty logs that it doesn't resolve, and it loads a
        // half-built track at ~1 fps with no way back to the menu.
        if (id == "suzuka24") return;
        if (!is_mounted(id)) return;
        for (const auto &t : out) {
            if (t.id == id) return;
        }
        out.push_back({id, prettify_token(id)});
    };

    const char *stock[] = {
        "melbourne", "shanghai", "suzuka", "bahrain", "jeddah", "miami", "imola", "monaco",
        "catalunya", "montreal", "red_bull_ring", "silverstone", "spa", "budapest", "zandvoort",
        "monza", "baku", "singapore", "austin", "mexico", "interlagos", "vegas", "qatar", "yas_marina"
    };
    for (const char *s : stock) add_track(s);

    if (swofty::db::available()) {
        swofty::db::Cursor cur{};
        if (swofty::db::open("tracks", &cur) == swofty::db::Result::ok) {
            do {
                if (const char *name = swofty::db::row_name(&cur)) {
                    add_track(name);
                }
            } while (swofty::db::step(&cur) == swofty::db::Result::ok);
        }
    }
    return out;
}

std::vector<SeatChoice> get_seats_for_era(const std::string &era_id, int64_t row_idx) {
    std::vector<SeatChoice> out;
    if (!swofty::db::available()) return out;

    std::map<int64_t, std::string> team_names;
    std::map<int64_t, std::string> driver_names;

    {
        swofty::db::Cursor cur{};
        if (swofty::db::open("entrants.teams", &cur) == swofty::db::Result::ok) {
            do {
                const char *meta = swofty::db::row_name(&cur);
                int64_t sid = -1;
                if (meta && cursor_int(&cur, "stat_id", sid) && sid >= 0) {
                    team_names[sid] = meta;
                }
            } while (swofty::db::step(&cur) == swofty::db::Result::ok);
        }
    }
    {
        swofty::db::Cursor cur{};
        if (swofty::db::open("entrants.drivers", &cur) == swofty::db::Result::ok) {
            do {
                const char *meta = swofty::db::row_name(&cur);
                int64_t sid = -1;
                if (meta && cursor_int(&cur, "stat_id", sid) && sid >= 0) {
                    driver_names[sid] = meta;
                }
            } while (swofty::db::step(&cur) == swofty::db::Result::ok);
        }
    }

    if (row_idx < 0) {
        swofty::db::Cursor cur{};
        int64_t idx = 0;
        if (swofty::db::open("entrants.entrant_lists", &cur) == swofty::db::Result::ok) {
            do {
                if (const char *name = swofty::db::row_name(&cur)) {
                    if (era_id == name) { row_idx = idx; break; }
                }
                ++idx;
            } while (swofty::db::step(&cur) == swofty::db::Result::ok);
        }
    }

    auto read_field = [](const std::string &key, int64_t &out_i, std::string &out_s) {
        swofty::db::Value v{};
        if (swofty::db::get(key, &v) != swofty::db::Result::ok) return false;
        if (v.tag == SWOFTY_DB_INT || v.tag == SWOFTY_DB_ID || v.tag == SWOFTY_DB_ENUM) {
            out_i = v.i;
            return true;
        }
        if (v.tag == SWOFTY_DB_STRING && v.s && *v.s) {
            out_s = v.s;
            return true;
        }
        return false;
    };

    std::vector<std::string> bases;
    if (row_idx >= 0) {
        bases.push_back("entrants.entrant_lists.entrant_list_" + std::to_string(row_idx) + ".entrant_");
    }
    bases.push_back("entrants.entrant_lists." + era_id + ".entrant_");

    for (const auto &base : bases) {
        for (int i = 0; i < 30; ++i) {
            std::string prefix = base + std::to_string(i);
            int64_t t_id = -1, d_id = -1;
            std::string t_raw, d_raw;
            read_field(prefix + ".team", t_id, t_raw);
            read_field(prefix + ".driver", d_id, d_raw);

            std::string tid;
            if (t_id >= 0 && team_names.count(t_id)) tid = team_names[t_id];
            else if (!t_raw.empty()) tid = t_raw;

            std::string did;
            if (d_id >= 0 && driver_names.count(d_id)) did = driver_names[d_id];
            else if (!d_raw.empty()) did = d_raw;

            if (!tid.empty()) {
                if (tid.find("safety") != std::string::npos) continue;
                out.push_back({
                    tid,
                    prettify_token(tid),
                    did,
                    did.empty() ? "Team Lead Driver" : prettify_token(did)
                });
            }
        }
        if (!out.empty()) break;
    }

    return out;
}

void reroll_seat_for_current_era() {
    auto seats = get_seats_for_era(g_cfg.era_id, g_cfg.era_row_index);
    if (!seats.empty()) {
        std::uniform_int_distribution<size_t> dist(0, seats.size() - 1);
        const auto &s = seats[dist(g_rng)];
        g_cfg.team_id = s.team_id;
        g_cfg.team_label = s.team_label;
        g_cfg.driver_id = s.driver_id;
        g_cfg.driver_label = s.driver_label;
    } else {
        g_cfg.team_id.clear();
        g_cfg.team_label = "Random Team";
        g_cfg.driver_id.clear();
        g_cfg.driver_label = "Random Driver";
    }
}

void reroll_era() {
    auto eras = get_installed_eras();
    if (!eras.empty()) {
        std::uniform_int_distribution<size_t> dist(0, eras.size() - 1);
        const auto &e = eras[dist(g_rng)];
        g_cfg.era_id = e.id;
        g_cfg.era_label = e.label;
        g_cfg.era_row_index = e.row_index;
    }
    reroll_seat_for_current_era();
}

void reroll_track() {
    auto tracks = get_installed_tracks();
    if (!tracks.empty()) {
        std::uniform_int_distribution<size_t> dist(0, tracks.size() - 1);
        const auto &t = tracks[dist(g_rng)];
        g_cfg.track_id = t.id;
        g_cfg.track_label = t.label;
    }
}

// Race distance presets: a share of the track's real Grand Prix lap count (the 2025 calendar's numbers; reverse layouts
// use their forward track's). Add-on tracks have no real F1 race to go by, so they use kAddonFullLaps as their full
// distance.
struct DistanceChoice {
    const char *label;
    float share; // 0 = custom (the slider)
};
constexpr DistanceChoice kDistances[] = {{"Custom", 0.0f}, {"Short", 0.25f}, {"Half", 0.5f}, {"Full Distance", 1.0f}};
constexpr int kDistancesCount = int(sizeof(kDistances) / sizeof(kDistances[0]));
constexpr int kAddonFullLaps = 60;

int real_race_laps(std::string id) {
    static const std::pair<const char *, int> kLaps[] = {
        {"melbourne", 58}, {"shanghai", 56}, {"suzuka", 53}, {"bahrain", 57}, {"jeddah", 50}, {"miami", 57},
        {"imola", 63}, {"monaco", 78}, {"catalunya", 66}, {"montreal", 70}, {"austria", 71}, {"silverstone", 52},
        {"spa_francorchamps", 44}, {"hungaroring", 70}, {"zandvoort", 72}, {"monza", 53}, {"baku", 51},
        {"singapore", 62}, {"texas", 56}, {"mexico", 71}, {"brazil", 71}, {"las_vegas", 50}, {"losail", 57},
        {"abu_dhabi", 58},
    };
    const std::string rev = "_reverse";
    if (id.size() > rev.size() && id.compare(id.size() - rev.size(), rev.size(), rev) == 0) id.resize(id.size() - rev.size());
    for (const auto &[name, laps] : kLaps)
        if (id == name) return laps;
    return 0;
}

// Laps for the current track under a distance preset (0 when the preset is Custom).
int preset_laps(const RaceConfig &cfg) {
    if (cfg.distance <= 0 || cfg.distance >= kDistancesCount) return 0;
    int full = real_race_laps(cfg.track_id);
    if (full <= 0) full = kAddonFullLaps;
    return std::clamp(int(std::lround(full * kDistances[cfg.distance].share)), 1, 100);
}

void sync_distance_laps() {
    if (int laps = preset_laps(g_cfg)) g_cfg.laps = float(laps);
}

void randomize_all() {
    reroll_era();
    reroll_track();
    // Mostly dry, about a third of races wet: clear 30, light cloud 20, overcast 15, light rain 15, heavy rain 12, storm 8.
    std::discrete_distribution<int> wdist({30, 20, 15, 15, 12, 8});
    g_cfg.weather = kWeathers[wdist(g_rng)];
    std::uniform_int_distribution<int> tdist(0, kTimesOfDayCount - 1);
    g_cfg.time_of_day = tdist(g_rng);
}

// --- Hooks for Laps & Weather ---
bool ensure_track_data_set(uint8_t *e, const uint8_t *tracks, void *list);
std::atomic<void *> g_launch_list{nullptr};
std::atomic<const uint8_t *> g_launch_tracks{nullptr};

uint64_t hook_build_settings(void *mode, uint8_t *settings) {
    if (g_stage.load() != static_cast<int>(LaunchStage::idle) && mode) {
        // Check the track data lookup again right before the game's build, in case the entrant list's season was read
        // again since the launch. Only for the entity this launch set up (same entrant list and track system).
        void *lst = nullptr;
        const uint8_t *trk = nullptr;
        if (rd(mode, ENTITY_LIST, lst) && rd(mode, ENTITY_TRACKS, trk) && lst && lst == g_launch_list.load() &&
            trk == g_launch_tracks.load())
            ensure_track_data_set(static_cast<uint8_t *>(mode), trk, lst);
    }
    uint64_t r = orig_build_settings ? orig_build_settings(mode, settings) : 0;
    int st = g_stage.load();
    {
        uint32_t sid0 = settings && swofty::mem::readable(settings, 8) ? *reinterpret_cast<uint32_t *>(settings + 4) : 0xffffffffu;
        if (st != static_cast<int>(LaunchStage::idle)) {
            int n = ++g_bss_calls;
            if (sid0 >= 15 && sid0 <= 17) ++g_bss_race;
            ilog("[instant-race] grid: BuildSessionSettings #%d of launch #%d (session id %u)", n, g_launch_no.load(), sid0);
        } else if (g_launch_no.load() > 0 && g_bss_idle_logged.fetch_add(1) < 12) {
            ilog("[instant-race] grid: BuildSessionSettings while no launch is running (session id %u)", sid0);
        }
    }
    if (st != static_cast<int>(LaunchStage::idle) && settings && swofty::mem::readable(settings, 0xe00)) {
        uint32_t sid = *reinterpret_cast<uint32_t *>(settings + 4);
        if (sid >= 15 && sid <= 17) {
            {
                uint32_t &flag = *reinterpret_cast<uint32_t *>(settings + 0x16c);
                if (flag) ilog("[instant-race] SessionSettings +0x16c was %u; cleared (prevents phantom lap at the start)", flag);
                flag = 0;
            }
            int want_laps = 5;
            {
                std::lock_guard lock(g_mutex);
                want_laps = static_cast<int>(std::lround(g_cfg.laps));
            }
            if (want_laps > 0 && want_laps <= 100) {
                auto &stored = *reinterpret_cast<uint32_t *>(settings + 0xdf0);
                uint32_t key = *reinterpret_cast<uint32_t *>(settings + 0xdf8);
                uint32_t game_laps = stored ^ key;
                g_game_laps.store(game_laps > 0 && game_laps < 1000 ? int(game_laps) : 0);
                stored = static_cast<uint32_t>(want_laps) ^ key;
                swofty::log("[instant-race] Applied custom race length: %d laps (the game's build had %u)", want_laps, game_laps);
            }
        }
    }
    return r;
}

// Starting fuel (Swofty RE: Game_CarSetup_SessionFuelLoad, float(setup), the session's fuel load in kg). The game
// fuels the race for its own Grand Prix race length (often 5 laps), worked out before our lap count replaces it, so
// the car ran dry about two thirds into a Short race. Scale the race fuel to our laps, keeping the game's margin
// (about two laps' worth: the formation lap and spare), and stay within an F1 tank.
using FuelLoadFn = float(void *setup);
FuelLoadFn *orig_fuel_load = nullptr;
float hook_fuel_load(void *setup) {
    float r = orig_fuel_load ? orig_fuel_load(setup) : 0.0f;
    if (g_stage.load() == static_cast<int>(LaunchStage::idle)) return r;
    int game = g_game_laps.load();
    int want = 0;
    {
        std::lock_guard lock(g_mutex);
        want = static_cast<int>(std::lround(g_cfg.laps));
    }
    if (game <= 0 || want <= 0 || want == game || !(r > 0.0f) || !std::isfinite(r)) {
        if (g_fuel_logs.fetch_add(1) < 4)
            ilog("[instant-race] Fuel: the game loaded %.2f kg (its laps %d, ours %d): left as it is", r, game, want);
        return r;
    }
    constexpr float kMargin = 2.0f, kTankKg = 110.0f;
    float scaled = std::min(r * (float(want) + kMargin) / (float(game) + kMargin), kTankKg);
    if (g_fuel_logs.fetch_add(1) < 4)
        ilog("[instant-race] Fuel: the game loaded %.2f kg for %d laps; %.2f kg for our %d laps", r, game, scaled, want);
    return scaled;
}

// Session weather (Swofty RE: Sess_GameWeatherSystem_SetupSession reads WeatherSessionSettings in rdx).
// mode +0xb14 (0 = custom slot blocks), slots[8] at +0xb74 + d*0x44 (d = P1 P2 P3 Q SS R R2 R3), each
// {f32 length, u32 count, f32 hold[6], f32 trans[6], u8 random[6] @+0x38, u8 type[6] @+0x3e}; type 0 clear .. 5 stormy.
// Dry / Wet write one fixed state into every dataset (clear / heavy rain) through the custom-slot path.
// Dry to Wet / Wet to Dry start the same way (the first state), then, after the game's build, rebuild each dataset
// with the game's own builder (Sess_Tod_BuildSessionDataset_Thunk, the way Swofty's weather chain does it): state i
// holds hold[i] s, then changes into state i + 1 over trans[i] s (>= 1 s); the last state holds to the end of the
// session and its transition is 0. Rain is only scripted when the track's type range can draw it, and a build with a
// non-finite state is replaced by one plain state.
struct WeatherStep {
    uint8_t type; // 0 clear, 1 light cloud, 2 overcast, 3 light rain, 4 heavy rain, 5 storm
    float hold;   // share of the race spent in it before the next change
    float trans;  // share of the race spent changing into the next state
};
constexpr WeatherStep kDryToWet[] = {{0, 0.30f, 0.08f}, {2, 0.07f, 0.08f}, {3, 0.10f, 0.07f}, {4, 0.0f, 0.0f}};
constexpr WeatherStep kWetToDry[] = {{4, 0.25f, 0.08f}, {3, 0.12f, 0.08f}, {2, 0.10f, 0.07f}, {0, 0.0f, 0.0f}};
constexpr float kSecondsPerLap = 95.0f; // rough lap time incl. the start, to place the change mid-race
const char *const kWeatherKinds[6] = {"clear", "light cloud", "overcast", "light rain", "heavy rain", "storm"};

using TodBuildFn = void(void *tod_owner, const uint8_t *types, const float *hold_s, const float *trans_s,
                        const uint8_t *random, uint32_t count, float len_s, uint16_t session, uint8_t use_count);
TodBuildFn *g_tod_build = nullptr;

// GameWeatherSystem +0x1d98 TimeOfDaySystem*, +0x1da8 TimeOfDayOwner* (owner +0x2d0 = its TimeOfDaySystem),
// +0x2f3a + d dataset_dynamic. TimeOfDaySystem: datasets at +0x26d8 + d*0xe00 (states at +0xb90, 16 bytes each:
// u32 type, f32 intensity, f32 end, f32 transition; state_total +0xdf0), dataset_length[d] +0xb350 + d*4,
// type ranges +0xb378 + t*0x14 (min +0xc, max +0x10), scripted_scenario +0x2654, exact_timing +0xb426,
// ready +0xb429/+0xb42a, random override +0xb42d, busy +0xb432, override active +0xb436 (Swofty session.hpp).
bool rebuild_dataset(uint8_t *ws, int d, const WeatherStep *steps, int n, float race_s, std::string &why) {
    uint8_t *tod = nullptr, *owner = nullptr;
    if (!swofty::mem::readable(ws + 0x1d98, 0x18) ||
        !(tod = *reinterpret_cast<uint8_t **>(ws + 0x1d98)) || !(owner = *reinterpret_cast<uint8_t **>(ws + 0x1da8)) ||
        !swofty::mem::readable(owner + 0x2d0, 8) || *reinterpret_cast<uint8_t **>(owner + 0x2d0) != tod ||
        !swofty::mem::readable(tod, 0xb438) || !swofty::mem::readable(ws + 0x2f3a, 8)) {
        why = "no TimeOfDaySystem";
        return false;
    }
    if (tod[0xb432]) { why = "TimeOfDaySystem busy"; return false; }
    if (!tod[0xb429] || !tod[0xb42a]) { why = "TimeOfDaySystem not ready"; return false; }
    if (tod[0xb436] && (tod[0xb42d] || tod[0x2654])) { why = "TimeOfDaySystem override active"; return false; }
    for (int i = 0; i < n; ++i) {
        uint8_t t = steps[i].type;
        if (t < 3) continue;
        float lo = *reinterpret_cast<float *>(tod + 0xb378 + t * 0x14 + 0xc);
        float hi = *reinterpret_cast<float *>(tod + 0xb378 + t * 0x14 + 0x10);
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo < 0 || lo > hi || hi > 1 || hi <= 0) {
            why = std::string("this track has no ") + kWeatherKinds[t];
            return false;
        }
    }
    float len = *reinterpret_cast<float *>(tod + 0xb350 + d * 4);
    if (!std::isfinite(len) || len < 60.0f || len > 36000.0f) len = d >= 5 ? 7200.0f : 3600.0f;
    uint8_t types[6] = {}, random[6] = {};
    float hold[6] = {}, trans[6] = {};
    float at = 0.0f;
    int m = 0;
    for (; m < n; ++m) {
        types[m] = steps[m].type;
        if (m + 1 == n) break;
        float h = std::max(1.0f, steps[m].hold * race_s), t = std::max(1.0f, steps[m].trans * race_s);
        if (at + h + t + 1.0f > len) break;
        hold[m] = h;
        trans[m] = t;
        at += h + t;
    }
    int count = m + 1;
    hold[count - 1] = std::max(1.0f, len - at);
    trans[count - 1] = 0.0f;

    uint8_t *set = tod + 0x26d8 + d * 0xe00;
    int first = (*reinterpret_cast<uint32_t *>(set + 0xdf0)) ? int(*reinterpret_cast<uint32_t *>(set + 0xb90)) : -1;
    auto finite = [&]() {
        uint32_t total = std::min<uint32_t>(*reinterpret_cast<uint32_t *>(set + 0xdf0), 38);
        for (uint32_t i = 0; i < total; ++i) {
            const float *f = reinterpret_cast<const float *>(set + 0xb90 + i * 16 + 4);
            for (int k = 0; k < 3; ++k)
                if (!std::isfinite(f[k]) || f[k] < 0.0f) return false;
            if (f[0] > 1.0f) return false;
        }
        return total > 0;
    };
    uint8_t exact = tod[0xb426];
    tod[0xb426] = 1;
    g_tod_build(owner, types, hold, trans, random, uint32_t(count), len, uint16_t(0x100 | d), 1);
    bool ok = finite();
    if (!ok) {
        uint8_t plain = uint8_t(first >= 0 && first < 6 ? first : steps[0].type);
        float one = len, zero = 0.0f;
        g_tod_build(owner, &plain, &one, &zero, random, 1, len, uint16_t(0x100 | d), 1);
    }
    tod[0xb426] = exact;
    ws[0x2f3a + d] = 0;
    if (!ok) why = "the game built an invalid state; kept one plain state";
    return ok;
}

void hook_weather_setup(void *ws, uint8_t *settings, void *config) {
    int st = g_stage.load();
    bool scripted = false, to_wet = false;
    float race_s = 0.0f;
    if (st != static_cast<int>(LaunchStage::idle) && settings && swofty::mem::readable(settings, 0xb74 + 8 * 0x44)) {
        bool want_wet = false;
        uint8_t wx_type = 0;
        int tod = 0;
        std::string weather;
        scripted = false;
        float laps = 5.0f;
        {
            std::lock_guard lock(g_mutex);
            weather = g_cfg.weather;
            wx_type = weather_type(weather);
            want_wet = wx_type >= 3;
            tod = g_cfg.time_of_day;
            laps = g_cfg.laps;
        }
        if (tod > 0 && tod < kTimesOfDayCount) {
            uint8_t preset = uint8_t(kTimesOfDay[tod].preset);
            uint8_t *track_cfg = nullptr;
            if (config && swofty::mem::readable(config, 8)) track_cfg = *static_cast<uint8_t **>(config);
            bool night_lighting = track_cfg && swofty::mem::readable(track_cfg + 0x29d, 1) && track_cfg[0x29d] != 0;
            if (preset == 7 && !night_lighting) {
                preset = 5;
                ilog("[instant-race] Start time: this track has no night lighting, using Sunset instead of Night");
            }
            ilog("[instant-race] Start time: game had flags %u/%u, session preset %u, race preset %u; setting %s (preset %u)",
                 unsigned(settings[0xb10]), unsigned(settings[0xb11]), unsigned(settings[0xb19]),
                 unsigned(settings[0xb28 + 5 * 8]), kTimesOfDay[tod].label, unsigned(preset));
            settings[0xb10] = 0; // not the game's per-session-type times
            settings[0xb11] = 0; // not a random pick
            settings[0xb19] = preset;
            for (int d = 0; d < 8; ++d) settings[0xb28 + d * 8] = preset;
        }
        uint8_t *race_slot = settings + 0xb74 + 5 * 0x44;
        ilog("[instant-race] Weather: game had exact %u, mode %u, race dataset length %.0f s, %u state(s)",
             unsigned(settings[0xb18]), *reinterpret_cast<uint32_t *>(settings + 0xb14),
             *reinterpret_cast<float *>(race_slot), *reinterpret_cast<uint32_t *>(race_slot + 4));
        scripted = (weather == "Dry to Wet" || weather == "Wet to Dry") && g_tod_build;
        if (scripted) {
            to_wet = weather == "Dry to Wet";
            want_wet = !to_wet; // the race starts in its first state; the rebuild after the game's build adds the change
            race_s = std::max(1.0f, laps) * kSecondsPerLap;
        }
        {
            uint8_t type = scripted ? (want_wet ? 4 : 0) : wx_type;
            settings[0xb18] = 0;
            *reinterpret_cast<uint32_t *>(settings + 0xb14) = 0;
            for (int d = 0; d < 8; ++d) {
                uint8_t *slot = settings + 0xb74 + d * 0x44;
                *reinterpret_cast<uint32_t *>(slot + 4) = 1;
                for (int k = 0; k < 6; ++k) {
                    slot[0x38 + k] = 0;
                    slot[0x3e + k] = type;
                }
            }
            ilog("[instant-race] Session weather set to %s (all datasets, custom slot), start time %s", kWeatherKinds[type < 6 ? type : 0],
                 time_of_day_label(tod));
        }
    }
    if (orig_weather_setup) orig_weather_setup(ws, settings, config);
    if (scripted && ws) {
        const WeatherStep *steps = to_wet ? kDryToWet : kWetToDry;
        int built = 0;
        std::string why;
        for (int d = 0; d < 8; ++d) {
            std::string w;
            if (rebuild_dataset(static_cast<uint8_t *>(ws), d, steps, 4, race_s, w)) ++built;
            else if (why.empty()) why = w;
        }
        if (built == 8)
            ilog("[instant-race] Session weather scripted %s over ~%.0f s: first change after %.0f s (about lap %.0f)",
                 to_wet ? "clear -> overcast -> light rain -> heavy rain" : "heavy rain -> light rain -> overcast -> clear",
                 race_s, steps[0].hold * race_s, std::floor(steps[0].hold * race_s / kSecondsPerLap) + 1.0f);
        else
            ilog("[instant-race] Changing weather: %d/8 sessions scripted (%s); the rest stay %s", built, why.c_str(),
                 to_wet ? "dry" : "wet");
    }
    // What the game made of it: GameWeatherSystem +0x1ec8/+0x1ecc = start hour/minute of the session being set up,
    // +0x1e94 / +0x1eac = the track's sunrise / sunset (hours).
    if (st != static_cast<int>(LaunchStage::idle) && ws && settings &&
        swofty::mem::readable(static_cast<uint8_t *>(ws) + 0x1e94, 0x40)) {
        auto *w = static_cast<uint8_t *>(ws);
        ilog("[instant-race] Session %u starts at %02d:%02d (sunrise %.2f, sunset %.2f)",
             unsigned(*reinterpret_cast<uint32_t *>(settings + 4)), *reinterpret_cast<int *>(w + 0x1ec8),
             *reinterpret_cast<int *>(w + 0x1ecc), *reinterpret_cast<float *>(w + 0x1e94),
             *reinterpret_cast<float *>(w + 0x1eac));
    }
}

// --- Flow State Machine Hook (site_FeFlow_AfterStateLink) ---
std::atomic<uint8_t *> g_fe_node{nullptr};
std::atomic<uint8_t *> g_fw_node{nullptr};

uint8_t *find_flow_node(uint32_t want);

void clear_menu_exit_latches() {
    if (uint8_t *fw = g_fw_node.load(); fw && swofty::mem::readable(fw + FW_EXIT_FLAG, 1) &&
        *reinterpret_cast<uint32_t *>(fw + STATE_HASH) == STATE_FRONTEND_WORLD && fw[FW_EXIT_FLAG]) {
        fw[FW_EXIT_FLAG] = 0;
        ilog("[instant-race] Menu world exit flag cleared");
    }
    uint8_t *fe = g_fe_node.load();
    if (!fe) fe = find_flow_node(STATE_FRONTEND);
    if (fe && swofty::mem::readable(fe + FE_INTERRUPT_FLAGS, 1) &&
        *reinterpret_cast<uint32_t *>(fe + STATE_HASH) == STATE_FRONTEND && (fe[FE_INTERRUPT_FLAGS] & FE_CLOSING_GAME)) {
        fe[FE_INTERRUPT_FLAGS] = uint8_t(fe[FE_INTERRUPT_FLAGS] & ~FE_CLOSING_GAME);
        ilog("[instant-race] Menu closing-game latch cleared");
    }
}

uint8_t *find_flow_node(uint32_t want) {
    uint8_t *root = g_flow_root.load();
    if (!root) return nullptr;
    std::vector<uint8_t *> todo{root};
    for (size_t i = 0; i < todo.size() && i < 1024; ++i) {
        uint8_t *n = todo[i];
        if (!n || !swofty::mem::readable(n, 0xc8)) continue;
        if (*reinterpret_cast<uint32_t *>(n + STATE_HASH) == want) return n;
        auto *cv = *reinterpret_cast<uint8_t **>(n + STATE_CHILDREN);
        if (!cv || !swofty::mem::readable(cv, 0x18)) continue;
        for (auto **p = *reinterpret_cast<uint8_t ***>(cv + 8), **e = *reinterpret_cast<uint8_t ***>(cv + 0x10);
             p && p < e && swofty::mem::readable(p, 8); ++p) {
            todo.push_back(*p);
        }
    }
    return nullptr;
}

void despawn_menu_scene() {
    if (g_scene_despawned || !g_addrs.despawn_main_scene) return;
    uint8_t *node = find_flow_node(STATE_FRONTEND);
    uint8_t *mgr = nullptr;
    if (node && rd(node, FRONTEND_SCENES, mgr) && mgr) {
        uint8_t *scenes = nullptr;
        if (rd(mgr, SCENES_MAIN, scenes) && scenes && swofty::mem::readable(scenes, MAIN_SPAWNED + 2)) {
            if (scenes[MAIN_SPAWNED] || scenes[MAIN_SPAWNED + 1]) {
                g_scene_despawned = true;
                reinterpret_cast<void (*)(void *)>(g_addrs.despawn_main_scene)(mgr);
                swofty::log("[instant-race] Despawned main menu scene");
            }
        }
    }
}

std::atomic<uint64_t> g_session_at_ms{0};
std::atomic<bool> g_session_visible{false};
uint32_t g_last_fade_mask = ~0u;
uint32_t g_last_fade_state = ~0u;
int g_fade_log_count = 0;

constexpr uint32_t FADE_STUCK_MENU = 0x80000000u; // Leftover menu fade bit seen when unwinding StateFrontend directly

void release_fade_mask() {
    if (g_fade_released || !g_addrs.game_root) return;
    auto *root = *reinterpret_cast<uint8_t **>(g_addrs.game_root);
    uint8_t *owner = nullptr;
    if (!root || !rd(root, 0xa0, owner) || !owner || !swofty::mem::readable(owner + 0x7b8, 0x18)) {
        ilog("[instant-race] release_fade_mask: game_root=%p root=%p owner=%p (unreadable)",
             (void *)g_addrs.game_root, root, owner);
        return;
    }
    g_fade_released = true;
    auto &mask = *reinterpret_cast<uint32_t *>(owner + 0x7b8);
    uint32_t was = mask;
    uint32_t st_before = *reinterpret_cast<uint32_t *>(owner + 0x7bc);
    mask &= ~(FADE_FRONTEND | 1u | FADE_STUCK_MENU);
    if (mask == 0) {
        *reinterpret_cast<uint32_t *>(owner + 0x7bc) = 3;
        owner[0x7cc] = 0;
    }
    *reinterpret_cast<uint32_t *>(owner + 0x7c4) = 0xd47eb226;
    uint32_t st_after = *reinterpret_cast<uint32_t *>(owner + 0x7bc);
    if (mask == 0 && st_after == 3) {
        g_session_visible.store(true);
    }
    ilog("[instant-race] Fade mask released on owner %p: %08x -> %08x, state %u -> %u",
         owner, was, mask, st_before, st_after);
}

void note_session_fade() {
    uint64_t sat = g_session_at_ms.load();
    if (!sat || g_fade_log_count >= 40 || !g_addrs.game_root) return;
    auto *root = *reinterpret_cast<uint8_t **>(g_addrs.game_root);
    uint8_t *owner = nullptr;
    if (!root || !rd(root, 0xa0, owner) || !owner || !swofty::mem::readable(owner + 0x7b8, 0x18)) return;
    auto &mask_ref = *reinterpret_cast<uint32_t *>(owner + 0x7b8);
    if (mask_ref & FADE_STUCK_MENU) {
        uint32_t before = mask_ref;
        mask_ref &= ~FADE_STUCK_MENU;
        if (mask_ref == 0 && *reinterpret_cast<uint32_t *>(owner + 0x7bc) != 3) {
            *reinterpret_cast<uint32_t *>(owner + 0x7bc) = 3;
            owner[0x7cc] = 0;
            *reinterpret_cast<uint32_t *>(owner + 0x7c4) = 0xd47eb226;
        }
        ilog("[instant-race] Cleared stuck menu fade bit: %08x -> %08x (state=%u)",
             before, mask_ref, *reinterpret_cast<uint32_t *>(owner + 0x7bc));
    }
    uint32_t mask = mask_ref;
    uint32_t state = *reinterpret_cast<uint32_t *>(owner + 0x7bc);
    if (mask == g_last_fade_mask && state == g_last_fade_state) return;
    bool faded_in = (mask == 0 && state == 3);
    g_last_fade_mask = mask;
    g_last_fade_state = state;
    ++g_fade_log_count;
    uint64_t ms = GetTickCount64() - sat;
    ilog("[instant-race] Fade owner %p: mask=%08x, state=%u (%llu ms into session)",
         owner, mask, state, static_cast<unsigned long long>(ms));
    if (faded_in && !g_session_visible.exchange(true)) {
        ilog("[instant-race] Broadcast faded in after %llu ms; removing cover", static_cast<unsigned long long>(ms));
    }
}

// Returns true once both sync nodes exist and have been set, false if they are not there yet (caller retries).
bool complete_load_sync(uint8_t *state) {
    uintptr_t base = swofty::api ? swofty::api->module_base() : 0;
    if (!base) return false;
    static uintptr_t fn_set_bool = 0;
    if (!fn_set_bool) fn_set_bool = bind_name("Vm_NodeSet_Bool");
    if (!fn_set_bool) {
        ilog("[instant-race] Vm_NodeSet_Bool binding not available; cannot complete the load sync");
        return false;
    }
    uint8_t *sync = state + LOAD_SYNC;
    if (!swofty::mem::readable(sync, 0xb0)) return false;
    const uint8_t one = 1;
    using NodeSetBool_t = void (*)(void *, const uint8_t *, int, int);
    bool all_found = true;
    for (size_t off : {size_t(0x80), size_t(0xa0)}) {
        void *node = *reinterpret_cast<void **>(sync + off);
        if (!node || !swofty::mem::readable(node, 0x60)) {
            all_found = false;
            continue;
        }
        if (!(static_cast<uint8_t *>(node)[0x26] & 1)) {
            reinterpret_cast<NodeSetBool_t>(fn_set_bool)(node, &one, 0, 0);
            swofty::log("[instant-race] Set StateLoadSession sync node +0x%zx (%p) to true", off, node);
        }
    }
    return all_found;
}

// Make the weekend's first session the Race (what direct_start.cpp's choose_session does).
// The weekend structure itself is already built once, earlier, in build_grand_prix_weekend().
uint8_t *current_weekend(void *modes);
void choose_race_session() {
    if (!g_addrs.game_manager) return;
    void *mgr = nullptr;
    if (!rd(reinterpret_cast<void *>(g_addrs.game_manager), 0, mgr) || !mgr) return;
    void *modes = nullptr;
    if (!rd(mgr, MANAGER_MODES, modes) || !modes) return;

    uint8_t *wk = current_weekend(modes);
    if (!wk) {
        swofty::log("[instant-race] choose_race_session: no current weekend to read");
        return;
    }
    auto *b = *reinterpret_cast<uint8_t **>(wk + 0x220);
    auto *e = *reinterpret_cast<uint8_t **>(wk + 0x228);
    auto &cur = *reinterpret_cast<uint32_t *>(wk + 0x238);
    if (b && e > b && swofty::mem::readable(b, 0xa0)) {
        if (cur != 0) {
            swofty::log("[instant-race] Weekend current session %u -> 0", cur);
            cur = 0;
        }
        auto &id = *reinterpret_cast<uint32_t *>(b + 0x8);
        ilog("[instant-race] Weekend session[0] id %u -> 15 (Race)", id);
        id = 15;
        int ts = g_track_stat.load();
        if (ts >= 0 && wk[0x278] != uint8_t(ts)) {
            swofty::log("[instant-race] Weekend track stat id %u -> %d", unsigned(wk[0x278]), ts);
            wk[0x278] = uint8_t(ts);
        }
    } else {
        ilog("[instant-race] choose_race_session: weekend has no readable sessions");
    }
}

bool build_grand_prix_weekend(const RaceConfig &cfg);

extern "C" uint32_t __fastcall on_flow_link_check(uint8_t *state, uint32_t link) {
    if (!state || !swofty::mem::readable(state, STATE_CHILD + 8)) return link;
    uint32_t hash = *reinterpret_cast<uint32_t *>(state + STATE_HASH);
    (void)STATE_CHILD;

    if (hash == STATE_APP && !g_flow_root.load()) {
        g_flow_root.store(state);
    }
    // Only mark main menu ready when STATE_FRONTEND (0x957880c6) itself is updating in active state (status == 2)
    if (hash == STATE_FRONTEND) g_fe_node.store(state);
    if (hash == STATE_FRONTEND && *reinterpret_cast<uint8_t *>(state + STATE_STATUS) == 2) {
        g_frontend_seen_ms.store(GetTickCount64());
        int cur_st = g_stage.load();
        if (cur_st == static_cast<int>(LaunchStage::idle) || cur_st == static_cast<int>(LaunchStage::done)) {
            if (g_in_session.exchange(false)) {
                g_stage.store(static_cast<int>(LaunchStage::idle));
                g_session_at_ms.store(0);
                g_session_visible.store(false);
                ilog("[instant-race] Returned to main menu (STATE_FRONTEND active); [F7] Instant Race re-armed");
            }
        }
    }

    int st = g_stage.load();
    if (st == static_cast<int>(LaunchStage::set_mode) || st == static_cast<int>(LaunchStage::wait_rounds)) {
        if (hash == STATE_FRONTEND || hash == STATE_FRONTEND_WORLD) {
            RaceConfig cfg_copy;
            {
                std::lock_guard lock(g_mutex);
                cfg_copy = g_cfg;
            }
            if (build_grand_prix_weekend(cfg_copy)) {
                g_stage.store(static_cast<int>(LaunchStage::leave_frontend));
                g_stage_ts = GetTickCount64();
                swofty::log("[instant-race] Weekend ready on flow thread; unwinding frontend states directly to session...");
            }
        }
        return link;
    }

    if (st == static_cast<int>(LaunchStage::leave_frontend)) {
        constexpr uint32_t DS_LINK_EXIT = 0x0eef86be;
        constexpr uint32_t DS_LINK_SESSION = 0x8c23ce89;
        bool has_child = *reinterpret_cast<void **>(state + STATE_CHILD) != nullptr;
        // 1. Raise eInterruptExit (0xe) on the shared interrupt queue (+0xbf8) and return DS_LINK_EXIT on child states of StateFrontendWorld
        if (hash != STATE_FRONTEND_WORLD && hash != STATE_APP) {
            uint8_t *queue = nullptr;
            if (swofty::mem::readable(state + 0xbf8, 8)) {
                queue = *reinterpret_cast<uint8_t **>(state + 0xbf8);
                if (queue && swofty::mem::readable(queue + 0x44, 4)) {
                    auto *qtype = reinterpret_cast<uint32_t *>(queue + 0x40);
                    if (*qtype == 0) {
                        *qtype = 0xe;
                        swofty::log("[instant-race] Raised eInterruptExit (0xe) on state %08x queue %p", hash, queue);
                    }
                }
            }
            if (!has_child) {
                swofty::log("[instant-race] Unwinding leaf state %08x with LINK_EXIT (%08x)", hash, DS_LINK_EXIT);
                return DS_LINK_EXIT;
            }
        }

        // 2. Once StateFrontendWorld (0x290b11ae) has no active child, force DS_LINK_SESSION (0x8c23ce89) to enter StateLoadSession!
        if (hash == STATE_FRONTEND_WORLD && !has_child) {
            // Clear eInterruptExit (0xe) from shared queue so StateLoadSession doesn't inherit it
            if (swofty::mem::readable(state + 0xbf8, 8)) {
                uint8_t *queue = *reinterpret_cast<uint8_t **>(state + 0xbf8);
                if (queue && swofty::mem::readable(queue + 0x44, 4)) {
                    auto *qtype = reinterpret_cast<uint32_t *>(queue + 0x40);
                    if (*qtype == 0xe) *qtype = 0;
                }
            }
            g_fw_node.store(state);
            clear_menu_exit_latches();
            swofty::log("[instant-race] StateFrontendWorld has no child -> returning DS_LINK_SESSION (%08x)!", DS_LINK_SESSION);
            g_stage.store(static_cast<int>(LaunchStage::loading_session));
            g_stage_ts = GetTickCount64();
            return DS_LINK_SESSION;
        }
        return link;
    } else if (st == static_cast<int>(LaunchStage::loading_session)) {
        // Walk the flow tree from the root down to StateLoadSession (the same walk direct_start.cpp does).
        uint8_t *root = g_flow_root.load();
        if (root && !g_load_sync_done) {
            for (uint8_t *n = root; n && swofty::mem::readable(n, STATE_CHILD + 8);
                 n = *reinterpret_cast<uint8_t **>(n + STATE_CHILD)) {
                if (*reinterpret_cast<uint32_t *>(n + STATE_HASH) != STATE_LOAD_SESSION) continue;

                if (!g_chose_sess.exchange(true)) {
                    ilog("[instant-race] StateLoadSession entered");
                    choose_race_session();
                }

                // StateLoadSession's exit waits for a sync point that the loading screen normally sets.
                // Once it has been exiting for a while, set it ourselves. Retry (like direct_start.cpp)
                // until both sync nodes exist, instead of giving up after one attempt.
                if (*reinterpret_cast<uint8_t *>(n + STATE_STATUS) != 3) {
                    g_load_exit_ts = 0;
                } else {
                    uint64_t now = GetTickCount64();
                    if (!g_load_exit_ts) g_load_exit_ts = now;
                    if (g_load_sync_tries < 10 && now - g_load_exit_ts >= 500ull * static_cast<uint64_t>(g_load_sync_tries + 1)) {
                        ++g_load_sync_tries;
                        g_load_sync_done = complete_load_sync(n);
                        ilog("[instant-race] StateLoadSession exiting for %llu ms: sync point is %s (try %d)",
                             static_cast<unsigned long long>(now - g_load_exit_ts),
                             g_load_sync_done ? "complete" : "not ready yet", g_load_sync_tries);
                    }
                }
                break;
            }
        }
        if (hash == STATE_SESSION && *reinterpret_cast<uint8_t *>(state + STATE_STATUS) == 2) {
            if (!g_session_at_ms.load()) {
                g_session_at_ms.store(GetTickCount64());
                ilog("[instant-race] STATE_SESSION active (game_root=%p)", (void *)g_addrs.game_root);
            }
            despawn_menu_scene();
            ilog("[instant-race] Main menu scene despawn %s", g_scene_despawned ? "done" : "not needed/not found");
            release_fade_mask();
            g_stage.store(static_cast<int>(LaunchStage::done));
            g_raced_once = true;
            clear_menu_exit_latches();
            ilog("[instant-race] Session active! Handover complete.");
            ilog("[instant-race] grid: launch #%d reached the session: BuildSessionSettings %d call(s), %d for a race "
                 "session; weekend laid out again: %s", g_launch_no.load(), g_bss_calls.load(), g_bss_race.load(),
                 g_relaid.load() ? "yes" : "no");
        }
    } else if (st == static_cast<int>(LaunchStage::done)) {
        note_session_fade();
    }
    return link;
}

// Runs at site_FeFlow_AfterStateLink: the flow state machine has just called the active state's update, which wrote
// the link it returns to [rsp+0x40]; rdi is the state. The next instruction loads that link for the compare with the
// stay link, so replacing [rsp+0x40] here changes where the flow goes.
void on_flow_site(SwoftyContext *c) {
    if (!c) return;
    auto *slot = reinterpret_cast<uint32_t *>(c->rsp + 0x40);
    uint32_t link = *slot;
    uint32_t next = on_flow_link_check(reinterpret_cast<uint8_t *>(c->rdi), link);
    if (next != link) *slot = next;
}

// --- Direct Grand Prix Weekend Construction ---
void *registry_call(uint8_t *entity, size_t slot, const void *a2, int a3) {
    if (!entity || !a2) return nullptr;
    auto *reg = entity + ENTITY_REGISTRY;
    if (!swofty::mem::readable(reg, 8)) return nullptr;
    auto **vt = *reinterpret_cast<void ***>(reg);
    if (!vt || !swofty::mem::readable(vt, slot + 8)) return nullptr;
    return reinterpret_cast<void *(*)(void *, const void *, int)>(vt[slot / 8])(reg, a2, a3);
}

std::atomic<uint8_t *> g_gp_mode{nullptr}; // the Grand Prix mode (entity+ENTITY_MODE) the weekend was built on

// Same chain the session load reads ([modes+0x88] mode > +0x260 entity > +0x1058 championship > ...), but starting
// from a given mode instead of the current one.
uint8_t *weekend_of_mode(uint8_t *mode) {
    uint8_t *entity = nullptr, *champ = nullptr, *sb = nullptr, *se = nullptr, *season = nullptr, **wb = nullptr, **we = nullptr, *wk = nullptr;
    uint32_t si = 0, wi = 0;
    if (!mode || !rd(mode, 0x260, entity) || !rd(entity, 0x1058, champ) ||
        !rd(champ, 0x548, sb) || !rd(champ, 0x550, se) || !rd(champ, 0x560, si) || se <= sb ||
        si >= size_t(se - sb) / 16 || !rd(sb, size_t(si) * 16 + 8, season) || !rd(season, 0x288, wb) ||
        !rd(season, 0x290, we) || !rd(season, 0x2a0, wi) || we <= wb || wi >= size_t(we - wb) ||
        !rd(wb, size_t(wi) * 8, wk) || !swofty::mem::readable(wk, 0x280)) {
        return nullptr;
    }
    return wk;
}


uint8_t *current_weekend(void *modes) {
    // The chain the session load reads ([modes+0x88] championship holder > +0x260 ...); the GP mode's own chain is
    // only a fallback for when that can't be read.
    uint8_t *holder = nullptr, *entity = nullptr, *champ = nullptr, *sb = nullptr, *se = nullptr, *season = nullptr,
            **wb = nullptr, **we = nullptr, *wk = nullptr;
    uint32_t si = 0, wi = 0;
    if (!rd(modes, 0x88, holder) || !rd(holder, 0x260, entity) || !rd(entity, 0x1058, champ) ||
        !rd(champ, 0x548, sb) || !rd(champ, 0x550, se) || !rd(champ, 0x560, si) || se <= sb ||
        si >= size_t(se - sb) / 16 || !rd(sb, size_t(si) * 16 + 8, season) || !rd(season, 0x288, wb) ||
        !rd(season, 0x290, we) || !rd(season, 0x2a0, wi) || we <= wb || wi >= size_t(we - wb) ||
        !rd(wb, size_t(wi) * 8, wk) || !swofty::mem::readable(wk, 0x280)) {
        return weekend_of_mode(g_gp_mode.load());
    }
    return wk;
}

size_t weekend_session_count(uint8_t *wk) {
    uint8_t *b = nullptr, *en = nullptr;
    if (!wk || !rd(wk, WK_SESSIONS, b) || !rd(wk, WK_SESSIONS + 8, en) || !b || en <= b) return 0;
    return size_t(en - b) / 0xa0;
}

void seat_player_on_grid(uint8_t *e, void *mgr, const std::string &want_team, const std::string &want_driver) {
    auto **begin = reinterpret_cast<uint8_t **>(e + ENTITY_ENTRANTS);
    if (!begin[0] || !begin[1] || begin[1] <= begin[0]) return;
    size_t n = size_t(begin[1] - begin[0]) / ENTRANT_SIZE;
    if (!n) return;

    std::map<uint32_t, std::string> teams, drivers;
    if (swofty::db::available()) {
        swofty::db::Cursor c{};
        if (swofty::db::open("entrants.teams", &c) == swofty::db::Result::ok) {
            do {
                const char *m = swofty::db::row_name(&c);
                int64_t sid = -1;
                if (m && cursor_int(&c, "stat_id", sid) && sid >= 0)
                    teams[uint32_t(sid)] = m;
            } while (swofty::db::step(&c) == swofty::db::Result::ok);
        }
        if (swofty::db::open("entrants.drivers", &c) == swofty::db::Result::ok) {
            do {
                const char *m = swofty::db::row_name(&c);
                int64_t sid = -1;
                if (m && cursor_int(&c, "stat_id", sid) && sid >= 0)
                    drivers[uint32_t(sid)] = m;
            } while (swofty::db::step(&c) == swofty::db::Result::ok);
        }
    }

    auto match_str = [](const std::string &hay, const std::string &needle) {
        if (needle.empty()) return true;
        if (hay == needle) return true;
        return hay.find(needle) != std::string::npos;
    };

    uint8_t *seat = nullptr;
    for (size_t i = 0; i < n; ++i) {
        uint8_t *r = begin[0] + i * ENTRANT_SIZE;
        bool human = (r[0x4b] & 0x10) != 0;
        std::string t_str = r[0x58] ? "" : teams[*reinterpret_cast<uint16_t *>(r + 0x5a)];
        std::string d_str = r[0x50] ? "" : drivers[*reinterpret_cast<uint16_t *>(r + 0x52)];
        if (!seat && !human && match_str(t_str, want_team) && match_str(d_str, want_driver)) {
            seat = r;
        }
    }
    if (!seat) {
        for (size_t i = 0; i < n; ++i) {
            uint8_t *r = begin[0] + i * ENTRANT_SIZE;
            std::string t_str = r[0x58] ? "" : teams[*reinterpret_cast<uint16_t *>(r + 0x5a)];
            if (match_str(t_str, want_team)) {
                seat = r;
                break;
            }
        }
    }
    if (!seat) {
        // The wanted team id may carry a different season prefix than the list that was loaded
        // (f1cg_f1_2017_haas-ferrari vs sw_f1_2017_haas-ferrari): match on the part after the year.
        std::string tail = want_team;
        size_t us = std::string::npos;
        for (size_t k = 0; k + 5 < tail.size(); ++k) {
            if (tail[k] == '2' && tail[k + 1] == '0' && std::isdigit(static_cast<unsigned char>(tail[k + 2])) &&
                std::isdigit(static_cast<unsigned char>(tail[k + 3])) && tail[k + 4] == '_') {
                us = k + 5;
                break;
            }
        }
        if (us != std::string::npos) {
            tail = tail.substr(us);
            for (size_t i = 0; i < n && !seat; ++i) {
                uint8_t *r = begin[0] + i * ENTRANT_SIZE;
                std::string t_str = r[0x58] ? "" : teams[*reinterpret_cast<uint16_t *>(r + 0x5a)];
                if (!tail.empty() && t_str.find(tail) != std::string::npos && !(r[0x4b] & 0x10)) seat = r;
            }
        }
    }
    if (!seat) {
        // Token match: f1cg_f1_2019_scuderia_toro_rosso-honda -> tokens {scuderia,toro,rosso,honda};
        // pick the entrant team string containing the most tokens (>=4 chars), ignoring generic ones.
        std::string tail = want_team;
        for (size_t k = 0; k + 5 < tail.size(); ++k) {
            if (tail[k] == '2' && tail[k + 1] == '0' && std::isdigit(static_cast<unsigned char>(tail[k + 2])) &&
                std::isdigit(static_cast<unsigned char>(tail[k + 3])) && tail[k + 4] == '_') { tail = tail.substr(k + 5); break; }
        }
        std::vector<std::string> toks;
        std::string cur;
        for (char ch : tail + "_") {
            if (ch == '_' || ch == '-' || ch == ' ') { if (cur.size() >= 4) toks.push_back(cur); cur.clear(); }
            else cur += char(std::tolower(static_cast<unsigned char>(ch)));
        }
        int best = 0;
        for (size_t i = 0; i < n; ++i) {
            uint8_t *r = begin[0] + i * ENTRANT_SIZE;
            if (r[0x4b] & 0x10) continue;
            std::string t_str = r[0x58] ? "" : teams[*reinterpret_cast<uint16_t *>(r + 0x5a)];
            {
                std::string nn;
                for (char c : t_str) if (std::isalnum(static_cast<unsigned char>(c))) nn += char(std::tolower(static_cast<unsigned char>(c)));
                t_str = nn;
            }
            int score = 0;
            bool first_tok = true;
            for (auto &tk : toks) {
                if (tk == "scuderia" || tk == "racing" || tk == "team") continue;
                // The first real token is the team (williams, mclaren...); later ones are usually the engine
                // supplier (mercedes, renault, honda...), which must not outweigh the team name.
                int w = first_tok ? 3 : 1;
                first_tok = false;
                if (t_str.find(tk) != std::string::npos) score += w;
            }
            {
                std::string nt;
                for (char c : tail) if (std::isalnum(static_cast<unsigned char>(c))) nt += char(std::tolower(static_cast<unsigned char>(c)));
                if (nt.size() >= 4 && t_str.find(nt) != std::string::npos) score += 3;
            }
            if (score > best) { best = score; seat = r; }
        }
        if (best < 3) {
            // The team name itself didn't match (at best an engine supplier did).
            uint8_t *engine_only = seat;
            seat = nullptr;
            // A team whose name is shorter than the 4-letter token minimum (HRT: f1cg_f1_2012_hrt-cosworth) is never
            // scored above: match its first word exactly against the entrant's slot name (sw12_hrt -> "hrt").
            // Only the first word (the team), never the engine supplier after it.
            std::string first_word;
            for (char ch : tail) {
                if (ch == '_' || ch == '-' || ch == ' ') break;
                first_word += char(std::tolower(static_cast<unsigned char>(ch)));
            }
            for (size_t i = 0; first_word.size() >= 3 && i < n && !seat; ++i) {
                uint8_t *r = begin[0] + i * ENTRANT_SIZE;
                if (r[0x4b] & 0x10) continue;
                std::string t_str = r[0x58] ? "" : teams[*reinterpret_cast<uint16_t *>(r + 0x5a)];
                size_t us = t_str.find('_');
                if (us != std::string::npos && t_str.substr(us + 1) == first_word) seat = r;
            }
            if (seat) ilog("[instant-race] Team slot by exact name '%s': found", first_word.c_str());
            // Then the slot aliases.
            // Swofty season lists keep each team in its modern "slot": Marussia/Manor/Virgin in the Haas slot,
            // Renault/Alpine in the Lotus slot, Racing Point/Aston Martin in Force India's, AlphaTauri/RB in Toro
            // Rosso's, Alfa Romeo/Kick in Sauber's (e.g. sw13_haas is the 2013 Marussia, sw18_lotus the 2018 Renault).
            static const std::pair<const char *, const char *> SLOT_ALIASES[] = {
                {"marussia", "haas"}, {"manor", "haas"}, {"virgin", "haas"},
                {"renault", "lotus"}, {"alpine", "lotus"},
                {"point", "force"}, {"aston", "force"},
                {"alphatauri", "toro"}, {"tauri", "toro"}, {"visa", "toro"},
                {"torro", "toro"},  // the game's own 2013/2016 ids spell it "torro_rosso"
                {"alfa", "sauber"}, {"kick", "sauber"}, {"stake", "sauber"},
            };
            const char *slot = nullptr;
            for (auto &tk : toks) {
                for (auto &al : SLOT_ALIASES) if (tk == al.first) { slot = al.second; break; }
                if (slot) break;
            }
            for (size_t i = 0; slot && i < n && !seat; ++i) {
                uint8_t *r = begin[0] + i * ENTRANT_SIZE;
                if (r[0x4b] & 0x10) continue;
                std::string t_str = r[0x58] ? "" : teams[*reinterpret_cast<uint16_t *>(r + 0x5a)];
                if (t_str.find(slot) != std::string::npos) seat = r;
            }
            if (slot) ilog("[instant-race] Team slot alias -> '%s': %s", slot, seat ? "found" : "none");
            if (!seat) seat = engine_only;
        }
        std::string all;
        for (size_t i = 0; i < n; ++i) {
            uint8_t *r = begin[0] + i * ENTRANT_SIZE;
            all += (all.empty() ? "" : ", ") + (r[0x58] ? std::string("?") : teams[*reinterpret_cast<uint16_t *>(r + 0x5a)]);
        }
        ilog("[instant-race] Team token match for '%s': %s; entrant teams: %s", want_team.c_str(),
             seat ? "found" : "none", all.c_str());
    }
    if (!seat) seat = begin[0];

    alignas(16) uint8_t id[0x40]{};
    void *users = mgr ? *reinterpret_cast<void **>(static_cast<uint8_t *>(mgr) + 0x140) : nullptr;
    void *user = users && swofty::mem::readable(users, 0x1c8) ? *reinterpret_cast<void **>(static_cast<uint8_t *>(users) + 0x1c0) : nullptr;
    void *prof = user && swofty::mem::readable(user, 0x1c8) ? *reinterpret_cast<void **>(static_cast<uint8_t *>(user) + 0x1c0) : nullptr;
    if (!prof) return;

    reinterpret_cast<void (*)(void *, void *)>((*reinterpret_cast<void ***>(prof))[0x38 / 8])(prof, id);
    void *bus = g_addrs.message_bus ? *reinterpret_cast<void **>(g_addrs.message_bus) : nullptr;
    void *mb = bus && swofty::mem::readable(bus, 0x10) ? *reinterpret_cast<void **>(static_cast<uint8_t *>(bus) + 8) : nullptr;
    alignas(16) void *msg[3] = {reinterpret_cast<void *>(g_addrs.vt_msg_begin), nullptr, nullptr};
    if (mb && g_addrs.post_begin && g_addrs.vt_msg_begin) {
        reinterpret_cast<void (*)(void *, void *)>(g_addrs.post_begin)(mb, msg);
    }

    e[0x2f8] = 1;
    for (size_t i = 0; i < n; ++i) {
        uint8_t *r = begin[0] + i * ENTRANT_SIZE;
        if (r != seat && (r[0x4b] & 0x10)) {
            *reinterpret_cast<uint32_t *>(r + 0x48) &= ~0x10000000u;
        }
    }
    std::memcpy(seat + 8, id, 0x40);
    *reinterpret_cast<uint32_t *>(seat + 0x48) = 0x10000000;

    msg[0] = reinterpret_cast<void *>(g_addrs.vt_msg_end);
    msg[1] = nullptr;
    msg[2] = e + 0x2d8;
    if (mb && g_addrs.post_end && g_addrs.vt_msg_end) {
        reinterpret_cast<void (*)(void *, void *)>(g_addrs.post_end)(mb, msg);
    }
    e[0x2f8] = 0;
    size_t seat_idx = size_t(seat - begin[0]) / ENTRANT_SIZE;
    std::string chosen_t = seat[0x58] ? "?" : teams[*reinterpret_cast<uint16_t *>(seat + 0x5a)];
    std::string chosen_d = seat[0x50] ? "?" : drivers[*reinterpret_cast<uint16_t *>(seat + 0x52)];
    ilog("[instant-race] Seated player in seat %zu/%zu: team=%s, driver=%s (wanted team=%s, driver=%s)",
         seat_idx, n, chosen_t.c_str(), chosen_d.c_str(), want_team.c_str(), want_driver.c_str());
}

// The session build reads the track's TRACK_DATA from the first data set (track system +0x68..+0x70, 0x30 rows)
// whose masks hold the session's setup mode (+0x20), the entrant list's season (+0x24, only when the list names one)
// and the weekend format (+0x28). If no set holds that mode and season together (a list that names a season its setup
// mode has no data set for, e.g. season_2026 in a mode only the 2023-2025 sets list), the game reads a null track and
// crashes on the loading screen. Returns the first matching set's track count, -1 when no set matches, -2 when the
// sets can't be read.
int track_data_set_items(const uint8_t *tracks, unsigned mode, bool season_used, unsigned season, unsigned format) {
    const uint8_t *b = nullptr, *end = nullptr;
    if (!rd(tracks, 0x68, b) || !rd(tracks, 0x70, end) || !b || end < b || (end - b) % 0x30) return -2;
    if (mode >= 32 || (season_used && season >= 32) || format >= 2) return -1;
    for (const uint8_t *s = b; s < end && s < b + 0x30 * 64; s += 0x30) {
        uint32_t m = 0, sz = 0, f = 0;
        if (!rd(s, 0x20, m) || !rd(s, 0x24, sz) || !rd(s, 0x28, f)) return -2;
        if (!((m >> mode) & 1) || (season_used && !((sz >> season) & 1)) || !((f >> format) & 1)) continue;
        const uint8_t *ib = nullptr, *ie = nullptr;
        if (!rd(s, 0x08, ib) || !rd(s, 0x10, ie) || !ib || ie < ib) return 0;
        int n = 0;
        for (const uint8_t *i = ib; i < ie && i < ib + 16 * 256; i += 16) {
            void *t = nullptr;
            if (rd(i, 8, t) && t) ++n;
        }
        return n;  // the game takes the first matching set and never tries a later one
    }
    return -1;
}

// A season the mode has track data for: the lowest season of the last data set (in row order) that lists the mode
// and the format, i.e. the newest stock season set for that mode. -1 when no set lists the mode.
int pick_track_data_season(const uint8_t *tracks, unsigned mode, unsigned format) {
    const uint8_t *b = nullptr, *end = nullptr;
    if (mode >= 32 || format >= 2 || !rd(tracks, 0x68, b) || !rd(tracks, 0x70, end) || !b || end < b) return -1;
    int pick = -1;
    for (const uint8_t *s = b; s < end && s < b + 0x30 * 64; s += 0x30) {
        uint32_t m = 0, sz = 0, f = 0;
        if (!rd(s, 0x20, m) || !rd(s, 0x24, sz) || !rd(s, 0x28, f)) break;
        if (!((m >> mode) & 1) || !((f >> format) & 1) || !sz) continue;
        for (int bit = 0; bit < 32; ++bit)
            if ((sz >> bit) & 1) { pick = bit; break; }
    }
    return pick;
}

// Make sure the session build will find track data for this setup mode and entrant list. When the list's season has
// no data set in this mode, the list runs as a season that has one. Returns false when the mode has no data set.
bool ensure_track_data_set(uint8_t *e, const uint8_t *tracks, void *list) {
    void *gsm = nullptr;
    uint8_t mode = 0;
    uint8_t *lst = static_cast<uint8_t *>(list);
    if (!rd(e, 0x2b0, gsm) || !gsm || !rd(gsm, 0x80, mode) || !lst || !swofty::mem::readable(lst + 0xb0, 2)) {
        ilog("[instant-race] Track data check skipped (mode or list not readable)");
        return true;
    }
    const unsigned season = lst[0xb0];
    const bool season_used = lst[0xb1] != 0;
    int n = track_data_set_items(tracks, mode, season_used, season, 0);
    if (n == -2) {
        ilog("[instant-race] Track data check skipped (data sets not readable)");
        return true;
    }
    if (n > 0) {
        ilog("[instant-race] Track data: mode %u, season %u%s -> a data set with %d track(s)",
             unsigned(mode), season, season_used ? "" : " (not used)", n);
        return true;
    }
    // Run the list as a season the mode has track data for. Leaving the season out of the query instead is not safe:
    // the same query picks the track's extra layer worlds, and with no season every season's layers load (crash).
    int pick = pick_track_data_season(tracks, mode, 0);
    if (pick >= 0 && track_data_set_items(tracks, mode, true, unsigned(pick), 0) > 0) {
        lst[0xb0] = uint8_t(pick);
        lst[0xb1] = 1;
        ilog("[instant-race] Track data: no data set for mode %u with the list's season %u%s; the list runs as season %d, "
             "which has one, instead of a crash on loading", unsigned(mode), season, season_used ? "" : " (not used)", pick);
        return true;
    }
    ilog("[instant-race] Track data: no data set for mode %u (season %u%s): launch refused", unsigned(mode), season,
         season_used ? "" : ", not used");
    return false;
}

bool build_grand_prix_weekend(const RaceConfig &cfg) {
    resolve_all_game_addrs();

    if (!g_addrs.game_manager || !g_addrs.set_mode || !g_addrs.mode_by_predicate || !g_addrs.gp_predicate) {
        ilog("[instant-race] Missing core game manager / mode bindings (game_manager=%p set_mode=%p mode_by_predicate=%p gp_predicate=%p)",
             (void *)g_addrs.game_manager, (void *)g_addrs.set_mode, (void *)g_addrs.mode_by_predicate, (void *)g_addrs.gp_predicate);
        swofty::overlay::ui::toast(g_ui_state, "Instant Race needs a newer Swofty Launcher: update it and restart the game.");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }

    void *mgr = nullptr;
    if (!rd(reinterpret_cast<void *>(g_addrs.game_manager), 0, mgr) || !mgr) {
        swofty::log("[instant-race] g_Game_Manager is null");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }
    void *modes = nullptr;
    if (!rd(mgr, MANAGER_MODES, modes) || !modes) {
        swofty::log("[instant-race] GameModes manager is null");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }

    void *pred = reinterpret_cast<void *>(g_addrs.gp_predicate);
    uint8_t *e = reinterpret_cast<ModeByPredicateFn *>(g_addrs.mode_by_predicate)(modes, &pred);
    if (!e || !swofty::mem::readable(e, ENTITY_TRACKS + 8)) {
        swofty::log("[instant-race] Grand Prix mode entity not found");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }

    int st = g_stage.load();
    if (st == static_cast<int>(LaunchStage::set_mode)) {
        reinterpret_cast<SetModeFn *>(g_addrs.set_mode)(modes, *reinterpret_cast<void **>(e + ENTITY_MODE));
        g_stage_ts = GetTickCount64();
        g_stage.store(static_cast<int>(LaunchStage::wait_rounds));
        swofty::log("[instant-race] Set Grand Prix mode current; waiting for rounds component...");
        return false;
    }

    if (!g_addrs.rounds_component || !g_addrs.set_setup_mode ||
        !g_addrs.rounds_clear || !g_addrs.add_track || !g_addrs.build_season || !g_addrs.fill_entrants) {
        ilog("[instant-race] Missing Grand Prix weekend setup bindings (rounds_clear=%p add_track=%p build_season=%p fill_entrants=%p)",
             (void *)g_addrs.rounds_clear, (void *)g_addrs.add_track, (void *)g_addrs.build_season, (void *)g_addrs.fill_entrants);
        swofty::overlay::ui::toast(g_ui_state, "Instant Race needs a newer Swofty Launcher: update it and restart the game.");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }

    uint8_t *es = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(mgr) + MANAGER_ENTRANTS);
    auto *lists = *reinterpret_cast<uint8_t **>(es + ES_LISTS);
    auto *lists_end = *reinterpret_cast<uint8_t **>(es + ES_LISTS + 8);
    size_t list_count = (lists && lists_end > lists) ? size_t(lists_end - lists) / 16 : 0;

    std::string resolved_list_name;
    uint8_t resolved_list_id = 0xff;
    auto read_db_int = [](const std::string &key, int64_t &out) -> bool {
        swofty::db::Value v{};
        if (swofty::db::get(key, &v) != swofty::db::Result::ok) return false;
        if (v.tag == SWOFTY_DB_INT || v.tag == SWOFTY_DB_ID || v.tag == SWOFTY_DB_ENUM) {
            out = v.i;
            return true;
        }
        if (v.tag == SWOFTY_DB_STRING && v.s && *v.s) {
            char *endp = nullptr;
            long long parsed = std::strtoll(v.s, &endp, 10);
            if (endp != v.s) {
                out = parsed;
                return true;
            }
        }
        return false;
    };

    auto resolve_list_by_name = [&](const std::string &target_name) -> void * {
        if (target_name.empty() || !lists || !list_count || !swofty::db::available()) return nullptr;
        int64_t count = 0;
        read_db_int("entrants.entrant_lists._entrant_list_instancecount_", count);
        for (int64_t i = 0; i < count && i < 4096; ++i) {
            std::string row = "entrants.entrant_lists.entrant_list_" + std::to_string(i);
            swofty::db::Value meta_v{};
            if (swofty::db::get(row + "._metadata_", &meta_v) != swofty::db::Result::ok ||
                meta_v.tag != SWOFTY_DB_STRING || !meta_v.s) {
                continue;
            }
            if (target_name == meta_v.s) {
                int64_t sid = -1;
                bool have_sid = read_db_int(row + ".stat_id", sid);
                ilog("[instant-race] list lookup '%s': row=%s stat_id=%s%lld list_count=%zu ptr=%p",
                     target_name.c_str(), row.c_str(), have_sid ? "" : "(unreadable) ", (long long)sid, list_count,
                     (have_sid && sid >= 0 && size_t(sid) < list_count) ? *reinterpret_cast<void **>(lists + size_t(sid) * 16 + 8) : nullptr);
                if (have_sid && sid >= 0 && size_t(sid) < list_count) {
                    void *p = *reinterpret_cast<void **>(lists + size_t(sid) * 16 + 8);
                    if (p) {
                        resolved_list_name = target_name;
                        resolved_list_id = static_cast<uint8_t>(sid);
                        return p;
                    }
                }
                break;
            }
        }
        return nullptr;
    };

    {
        // Diagnostic: which entrant lists exist and which of them the game has loaded.
        int64_t cnt = 0;
        read_db_int("entrants.entrant_lists._entrant_list_instancecount_", cnt);
        ilog("[instant-race] entrant lists in DB: %lld, loaded slots in EntrantsSystem: %zu", (long long)cnt, list_count);
        for (int64_t i = 0; i < cnt && i < 200; ++i) {
            std::string row = "entrants.entrant_lists.entrant_list_" + std::to_string(i);
            swofty::db::Value mv{};
            int64_t sid = -1;
            if (swofty::db::get(row + "._metadata_", &mv) != swofty::db::Result::ok || mv.tag != SWOFTY_DB_STRING || !mv.s) continue;
            std::string nm = mv.s;
            if (nm.find("f1_20") == std::string::npos && nm.find("sw_") == std::string::npos) continue;
            bool ok = read_db_int(row + ".stat_id", sid);
            void *pp = (ok && sid >= 0 && size_t(sid) < list_count) ? *reinterpret_cast<void **>(lists + size_t(sid) * 16 + 8) : nullptr;
            (void)pp;
        }
    }

    void *list = resolve_list_by_name(cfg.era_id);
    if (!list && cfg.era_id.starts_with("f1cg_f1_")) {
        std::string yr = cfg.era_id.substr(8);
        // The Swofty season's loaded list is named sw_f1_<year>; the f1cg_f1_<year> list is not loaded by the game.
        list = resolve_list_by_name("sw_f1_" + yr);
        if (!list) list = resolve_list_by_name("sw_" + yr);
        if (!list) list = resolve_list_by_name("f1_" + yr);
    }
    if (!list && cfg.era_id.starts_with("f1_") && cfg.era_id.size() == 7) {
        // e.g. f1_2024 is in the DB but not loaded; the Swofty season list sw_f1_2024 is.
        list = resolve_list_by_name("sw_" + cfg.era_id);
    }
    if (!list) {
        ilog("[instant-race] Warning: era list '%s' not found via list_stat_id; falling back to f1_2025", cfg.era_id.c_str());
        list = resolve_list_by_name("f1_2025");
    }
    if (!list && list_count > 0) {
        for (size_t i = 0; i < list_count; ++i) {
            void *p = *reinterpret_cast<void **>(lists + i * 16 + 8);
            if (p) {
                list = p;
                resolved_list_name = "slot_" + std::to_string(i);
                resolved_list_id = static_cast<uint8_t>(i);
                break;
            }
        }
    }
    if (!list) {
        ilog("[instant-race] No loaded entrant list found in EntrantsSystem (%zu slots)", list_count);
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }
    ilog("[instant-race] Resolved entrant list '%s' -> '%s' (stat_id=%u, ptr=%p)",
         cfg.era_id.c_str(), resolved_list_name.c_str(), unsigned(resolved_list_id), list);

    auto **evt = *reinterpret_cast<void ***>(e);
    auto *modes_vec = reinterpret_cast<uint8_t *(*)(void *)>(evt[0x1d0 / 8])(e);
    uint8_t *picked_gsm = nullptr;
    uint8_t *fallback_gsm = nullptr;
    for (auto **p = *reinterpret_cast<uint8_t ***>(modes_vec + 8), **end = *reinterpret_cast<uint8_t ***>(modes_vec + 0x10);
         p && p != end; ++p) {
        uint8_t *m = *p;
        void *holder = m && swofty::mem::readable(m, 0xe0) ? *reinterpret_cast<void **>(m + 0xd8) : nullptr;
        const char *name = holder ? *reinterpret_cast<const char **>(static_cast<uint8_t *>(holder) + 0x10) : nullptr;
        if (!name) continue;
        if (std::strcmp(name, "swofty_f1") == 0) fallback_gsm = m;

        int64_t setups = *reinterpret_cast<int64_t *>(m + 0x70);
        for (int64_t i = 0; i < setups && i < 12 && !picked_gsm; ++i) {
            uint8_t *esm = *reinterpret_cast<uint8_t **>(m + 0x10 + size_t(i) * 8);
            if (!esm || !swofty::mem::readable(esm, 0x238)) continue;
            int64_t n = *reinterpret_cast<int64_t *>(esm + 0x230);
            for (int64_t k = 0; k < n && k < 32; ++k) {
                if (*reinterpret_cast<void **>(esm + 0x130 + size_t(k) * 8) == list) {
                    picked_gsm = m;
                    break;
                }
            }
        }
    }
    uint8_t *gsm = picked_gsm ? picked_gsm : fallback_gsm;
    if (!gsm) {
        swofty::log("[instant-race] No compatible GameSetupMode found");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }

    // 1. Set GameSetupMode & EntrantList BEFORE querying rounds_component (matches direct_start.cpp)
    reinterpret_cast<void (*)(void *, void *, int)>(g_addrs.set_setup_mode)(e, gsm, 0);
    *reinterpret_cast<void **>(e + ENTITY_LIST) = list;

    void *rounds = registry_call(e, 0x10, reinterpret_cast<void *>(g_addrs.rounds_component), 0);
    if (!rounds) {
        if (GetTickCount64() - g_stage_ts > 8000) {
            swofty::log("[instant-race] Timeout waiting for Grand Prix rounds component");
            g_stage.store(static_cast<int>(LaunchStage::idle));
        }
        return false;
    }

    uint8_t *tracks = *reinterpret_cast<uint8_t **>(e + ENTITY_TRACKS);
    uint8_t *track = nullptr;
    if (tracks && swofty::mem::readable(tracks, 0x38)) {
        for (auto *q = *reinterpret_cast<uint8_t **>(tracks + 0x28), *qend = *reinterpret_cast<uint8_t **>(tracks + 0x30);
             q && q < qend; q += 16) {
            auto *rec = *reinterpret_cast<uint8_t **>(q + 8);
            const char *name = rec && swofty::mem::readable(rec, 0x80) ? *reinterpret_cast<const char **>(rec + 0x78) : nullptr;
            if (name && cfg.track_id == name) {
                track = rec;
                break;
            }
        }
    }
    if (!track) {
        ilog("[instant-race] Track '%s' not found in Grand Prix track system - aborting launch", cfg.track_id.c_str());
        swofty::overlay::ui::toast(g_ui_state, "Track '" + cfg.track_label + "' not mounted yet - reroll track!");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }

    g_launch_list.store(list);
    g_launch_tracks.store(tracks);
    if (!ensure_track_data_set(e, tracks, list)) {
        swofty::overlay::ui::toast(g_ui_state, "This season has no track data in a Grand Prix - pick another season.");
        g_stage.store(static_cast<int>(LaunchStage::idle));
        return false;
    }

    reinterpret_cast<void (*)(void *, void *)>(g_addrs.rounds_clear)(nullptr, static_cast<uint8_t *>(rounds) + 0x298);
    reinterpret_cast<void (*)(void *, void *)>(g_addrs.add_track)(rounds, track);

    alignas(16) static uint8_t stub[0x400], holder[0x40];
    std::memset(stub, 0, sizeof(stub));
    std::memset(holder, 0, sizeof(holder));
    *reinterpret_cast<void **>(stub + 0x270) = e + ENTITY_REGISTRY;
    *reinterpret_cast<void **>(stub + 0x290) = rounds;
    *reinterpret_cast<void **>(holder + 0x18) = track;
    *reinterpret_cast<void **>(stub + 0x298) = holder;
    // Hand the weekend service the session plan BEFORE the season is built, as the game's own Grand Prix menu does
    // (its calls come while no weekend exists yet); the season build then creates the weekend from that plan. Calling
    // it only after the build left the weekend without sessions (ghost grid, phantom first lap) unless an older
    // plan happened to be left in the service.
    auto hand_plan = [&](const char *when) {
        uint64_t plan_race_kind = 3;
        char la[MAX_PATH] = {};
        if (GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la)) {
            std::string f = std::string(la) + "\\Swofty\\instant_race_kind7.txt";
            if (GetFileAttributesA(f.c_str()) != INVALID_FILE_ATTRIBUTES) plan_race_kind = 7;
        }
        void *p_alloc = nullptr, *p_gsm = nullptr;
        rd(rounds, 0x210, p_alloc);
        rd(e, 0x2b0, p_gsm);
        void *p_svc = g_addrs.weekend_service
            ? registry_call(e, 0x10, reinterpret_cast<void *>(g_addrs.weekend_service), 1) : nullptr;
        if (p_alloc && p_gsm && p_svc && g_addrs.weekend_structure && swofty::mem::readable(p_svc, 8)) {
            alignas(16) uint8_t plan[0x38]{};
            *reinterpret_cast<void **>(plan + 8) = p_alloc;
            *reinterpret_cast<void **>(plan + 0x10) = p_alloc;
            reinterpret_cast<void (*)(void *, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(
                g_addrs.weekend_structure)(plan, p_gsm, 0, 0, plan_race_kind, 0, 1, 0, 0);
            auto **pvt = *reinterpret_cast<void ***>(p_svc);
            if (pvt && swofty::mem::readable(pvt, 0x270)) {
                reinterpret_cast<void (*)(void *, void *, int)>(pvt[0x268 / 8])(p_svc, plan, 1);
                ilog("[instant-race] Session plan handed to the weekend service %s", when);
            }
        } else {
            ilog("[instant-race] Session plan NOT handed over %s (alloc=%p gsm=%p svc=%p)", when, p_alloc, p_gsm, p_svc);
        }
    };
    hand_plan("before the season build");
    reinterpret_cast<void (*)(void *)>(g_addrs.build_season)(stub);
    {
        uint8_t *gp_mode = nullptr, *cur_mode = nullptr;
        rd(e, ENTITY_MODE, gp_mode);
        rd(modes, 0x88, cur_mode);
        g_gp_mode.store(gp_mode);
        if (cur_mode != gp_mode)
            ilog("[instant-race] Note: the game's current mode (%p) is not the Grand Prix mode (%p) after the switch", cur_mode, gp_mode);
    }

    g_track_stat.store(track[1]);
    uint8_t *wk = current_weekend(modes);
    if (wk) wk[WK_TRACK] = track[1];
    else ilog("[instant-race] current weekend not readable after season build (will retry at load)");

    if (evt && swofty::mem::readable(evt, 0xf0)) {
        reinterpret_cast<void (*)(void *)>(evt[0xe8 / 8])(e);
    }
    void *alloc = nullptr, *cur_gsm = nullptr;
    rd(rounds, 0x210, alloc);
    rd(e, 0x2b0, cur_gsm);
    void *svc = g_addrs.weekend_service
        ? registry_call(e, 0x10, reinterpret_cast<void *>(g_addrs.weekend_service), 1)
        : nullptr;
    uint64_t race_kind = 3;
    {
        char la[MAX_PATH] = {};
        if (GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la)) {
            std::string f = std::string(la) + "\\Swofty\\instant_race_kind7.txt";
            if (GetFileAttributesA(f.c_str()) != INVALID_FILE_ATTRIBUTES) race_kind = 7;
        }
        ilog("[instant-race] Weekend race kind = %llu", (unsigned long long)race_kind);
    }
    if (!(alloc && cur_gsm && svc && g_addrs.weekend_structure && swofty::mem::readable(svc, 8))) {
        ilog("[instant-race] Weekend structure NOT built (alloc=%p gsm=%p svc=%p fn=%p)", alloc, cur_gsm, svc,
             (void *)g_addrs.weekend_structure);
    }
    if (alloc && cur_gsm && svc && g_addrs.weekend_structure && swofty::mem::readable(svc, 8)) {
        alignas(16) uint8_t wlist[0x38]{};
        *reinterpret_cast<void **>(wlist + 8) = alloc;
        *reinterpret_cast<void **>(wlist + 0x10) = alloc;
        reinterpret_cast<void (*)(void *, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(
            g_addrs.weekend_structure)(wlist, cur_gsm, 0, 0, race_kind, 0, 1, 0, 0);
        auto **svt = *reinterpret_cast<void ***>(svc);
        if (svt && swofty::mem::readable(svt, 0x270)) {
            reinterpret_cast<void (*)(void *, void *, int)>(svt[0x268 / 8])(svc, wlist, 1);
        }
    }

    alignas(16) static uint8_t blk[0x190];
    std::memset(blk, 0, sizeof(blk));
    *reinterpret_cast<uint64_t *>(blk + 0x180) = 1;
    reinterpret_cast<void (*)(void *, void *, void *, int)>(g_addrs.fill_entrants)(es, e, blk, 0);
    seat_player_on_grid(e, mgr, cfg.team_id, cfg.driver_id);

    wk = current_weekend(modes);
    if (wk && weekend_session_count(wk) == 0) {
        // A Grand Prix weekend already in the mode that was never played (a profile with no Grand Prix played) is
        // reset by the season build: its sessions are cleared and nothing lays them out again, so the race would
        // have no session (phantom first lap, fastest-lap spam). Lay the season's weekends out from the weekend
        // service's plan, as the game's weekend setup does (Sess_WeekendSetup_LayoutWeekends).
        static uintptr_t layout = 0;
        if (!layout) layout = bind_name("Sess_WeekendSetup_LayoutWeekends");
        uint8_t *lsvc = g_addrs.weekend_service
            ? static_cast<uint8_t *>(registry_call(e, 0x10, reinterpret_cast<void *>(g_addrs.weekend_service), 1)) : nullptr;
        void *svc_reg = nullptr, *svc_ent = nullptr;
        if (lsvc) {
            rd(lsvc, 0x270, svc_reg);
            rd(lsvc, 0x278, svc_ent);
        }
        if (layout && lsvc && svc_reg && svc_ent == e) {
            hand_plan("before laying out the weekend");
            reinterpret_cast<void (*)(void *)>(layout)(lsvc);
            wk = current_weekend(modes);
            g_relaid.store(true);
            ilog("[instant-race] The weekend had no sessions after the build; laid out again: %zu session(s)",
                 weekend_session_count(wk));
        } else {
            ilog("[instant-race] The weekend has no sessions and could not be laid out (layout %p, service %p, entity %p)",
                 (void *)layout, lsvc, svc_ent);
        }
    }
    if (!wk) ilog("[instant-race] current weekend not readable after entrants (will retry at load)");
    if (wk && swofty::mem::readable(wk, WK_TRACK + 4)) {
        wk[WK_TRACK] = track[1];
        *reinterpret_cast<uint32_t *>(wk + WK_CURRENT) = 0;
        uint8_t *sb = nullptr, *se = nullptr;
        if (rd(wk, WK_SESSIONS, sb) && rd(wk, WK_SESSIONS + 8, se) && sb && se > sb && swofty::mem::readable(sb, SESSION_ID + 4)) {
            uint32_t cur_sid = *reinterpret_cast<uint32_t *>(sb + SESSION_ID);
            if (cur_sid < 15 || cur_sid > 17) {
                *reinterpret_cast<uint32_t *>(sb + SESSION_ID) = 15;
            }
            ilog("[instant-race] Weekend session[0] id=%u (track_stat_id=%u)",
                 *reinterpret_cast<uint32_t *>(sb + SESSION_ID), unsigned(wk[WK_TRACK]));
        }
    }

    ilog("[instant-race] Grand Prix weekend built: era=%s, track=%s, team=%s, driver=%s, laps=%.0f",
         cfg.era_id.c_str(), cfg.track_id.c_str(), cfg.team_id.c_str(), cfg.driver_id.c_str(), cfg.laps);
    return true;
}

// Log where every car is: the live classification order with each car's lap, lap distance and position on the map,
// and how many cars stand within 3 m of another one (cars stacked on the start line instead of in grid slots).
void log_grid_snapshot(int which) {
    namespace live = swofty::live;
    auto ses = live::session();
    if (!live::available() || !ses) {
        ilog("[instant-race] grid: snapshot %d: live state not available", which);
        return;
    }
    auto ord = live::order();
    std::vector<live::Car> cars;
    for (uint32_t id : ord.ids)
        if (auto c = live::car(id)) cars.push_back(*c);
    if (cars.empty()) {
        for (uint32_t v = 0; v < live::max_cars; ++v)
            if (auto c = live::car(v); c && live::is_racer(*c)) cars.push_back(*c);
    }
    int stacked = 0;
    for (size_t i = 0; i < cars.size(); ++i) {
        for (size_t j = 0; j < cars.size(); ++j) {
            if (i == j || !live::has(cars[i], SWOFTY_LIVE_HAVE_WORLD) || !live::has(cars[j], SWOFTY_LIVE_HAVE_WORLD)) continue;
            float dx = cars[i].world_pos[0] - cars[j].world_pos[0], dz = cars[i].world_pos[2] - cars[j].world_pos[2];
            if (dx * dx + dz * dz < 9.0f) { ++stacked; break; }
        }
    }
    ilog("[instant-race] grid: snapshot %d (%u s after the session went live): type %u, %u car(s), %u racer(s), "
         "player vehicle %d, %zu in order, %d within 3 m of another car", which, which == 1 ? 5u : 20u, ses->type,
         ses->cars, ses->racers, ses->player, cars.size(), stacked);
    std::string line;
    for (size_t i = 0; i < cars.size(); ++i) {
        const auto &c = cars[i];
        char buf[160];
        std::snprintf(buf, sizeof buf, "%sP%d v%u%s lap %d %.0fm (%.0f,%.0f)", line.empty() ? "" : "; ", c.position,
                      c.vehicle, live::is_player(c) ? "*" : "", c.lap, c.lap_m, c.world_pos[0], c.world_pos[2]);
        line += buf;
        if ((i + 1) % 6 == 0 || i + 1 == cars.size()) {
            ilog("[instant-race] grid:   %s", line.c_str());
            line.clear();
        }
    }
}

void on_flow_frame(double, void *) {
    if (swofty::api && swofty::api->module_base()) {
        static uint64_t last_try = 0;
        if (!g_addrs.scanned && GetTickCount64() - last_try > 1000) {
            last_try = GetTickCount64();
            resolve_all_game_addrs();
        }
    }
    {   // Grid snapshots 5 s and 20 s after an Instant Race session goes live.
        uint64_t at = g_session_at_ms.load();
        if (at && g_launch_no.load() > 0 && g_grid_snaps < 2 && GetTickCount64() - at >= (g_grid_snaps ? 20000u : 5000u)) {
            ++g_grid_snaps;
            log_grid_snapshot(g_grid_snaps);
        }
    }
    int st = g_stage.load();
    if (st == static_cast<int>(LaunchStage::leave_frontend) && GetTickCount64() - g_stage_ts > 60000) {
        ilog("[instant-race] Timeout waiting for the menu to close (60 s)");
        g_stage.store(static_cast<int>(LaunchStage::idle));
    }
}

// --- AI difficulty -----------------------------------------------------------------------------------------------
// The game keeps menu settings in its options store (g_Game_OptionsStore; layout as Swofty core's box_options.cpp:
// entries at +0x288..+0x290 of 0x10 bytes with the option at +8; an option's values at +0x80..+0x88, 0x50 bytes each;
// index get/set through vtable slots 2/3). The AI difficulty option is the one with 111 values (0..110). We set it to
// the slider before the weekend is built and put the player's own value back when the session ends.
void *g_ai_option = nullptr;
int g_ai_saved_index = -1;

bool ai_code_ptr(const void *p) {
    auto a = reinterpret_cast<uintptr_t>(p);
    uintptr_t base = swofty::api ? swofty::api->module_base() : 0;
    return p && base && a >= base && a < base + 0x20000000 && swofty::mem::readable(p, 1);
}
void *ai_vfunc(const void *obj, size_t slot) {
    if (!swofty::mem::readable(obj, 8)) return nullptr;
    auto **vt = *static_cast<void **const *>(obj);
    if (!swofty::mem::readable(vt, (slot + 1) * 8) || !ai_code_ptr(vt[slot])) return nullptr;
    return vt[slot];
}
int ai_get_index(const void *opt) {
    auto *get = reinterpret_cast<int (*)(const void *, void *)>(ai_vfunc(opt, 2));
    if (!get) return -1;
    uint64_t scratch[2] = {};
    return get(opt, scratch);
}
bool ai_set_index(const void *opt, int idx) {
    auto *set = reinterpret_cast<void (*)(const void *, int)>(ai_vfunc(opt, 3));
    if (!set) return false;
    set(opt, idx);
    return true;
}
void *find_ai_difficulty_option() {
    uintptr_t find = (swofty::api && swofty::api->resolve) ? swofty::api->resolve("Game_OptionsStore_FindOption") : 0;
    uintptr_t slot = (swofty::api && swofty::api->resolve) ? swofty::api->resolve("g_Game_OptionsStore") : 0;
    if (!find || !slot || !swofty::mem::readable(reinterpret_cast<void *>(slot), 8)) {
        ilog("[instant-race] AI difficulty: options store not resolved on this build (find=%llx store=%llx)",
             (unsigned long long)find, (unsigned long long)slot);
        return nullptr;
    }
    auto *store = *reinterpret_cast<uint8_t **>(slot);
    if (!store || !swofty::mem::readable(store, 0x298)) return nullptr;
    auto *base = *reinterpret_cast<uint8_t **>(store + 0x288);
    auto *end = *reinterpret_cast<uint8_t **>(store + 0x290);
    if (!base || end <= base) return nullptr;
    size_t count = size_t(end - base) / 0x10;
    using FindFn = uint64_t(void *map, const char **name);
    // Option names from the game's own strings, most likely first.
    for (const char *nm : {"ai_difficulty", "ai_level", "difficulty", "aiDifficulty"}) {
        const char *name = nm;
        uint64_t idx = reinterpret_cast<FindFn *>(find)(store + 0x270, &name);
        if (idx >= count) { ilog("[instant-race] AI difficulty: no option named %s", nm); continue; }
        uint8_t *e = base + idx * 0x10;
        auto *opt = swofty::mem::readable(e, 0x10) ? *reinterpret_cast<uint8_t **>(e + 8) : nullptr;
        if (!opt || !swofty::mem::readable(opt, 0x90)) { ilog("[instant-race] AI difficulty: option %s unreadable", nm); continue; }
        auto *v = *reinterpret_cast<uint8_t **>(opt + 0x80);
        auto *ve = *reinterpret_cast<uint8_t **>(opt + 0x88);
        size_t nvals = (v && ve > v && size_t(ve - v) % 0x50 == 0) ? size_t(ve - v) / 0x50 : 0;
        int cur = ai_get_index(opt);
        ilog("[instant-race] AI difficulty: option %s found (store index %llu, %zu listed values, current %d)", nm,
             (unsigned long long)idx, nvals, cur);
        if (cur >= 0) return opt;
    }
    return nullptr;
}
[[maybe_unused]] void apply_ai_difficulty(int want) { // for the AI slider (ROADMAP 3)
    void *opt = find_ai_difficulty_option();
    if (!opt) { ilog("[instant-race] AI difficulty: option not found; the game's own setting is used"); return; }
    int before = ai_get_index(opt);
    if (before < 0) { ilog("[instant-race] AI difficulty: option has no readable index"); return; }
    want = std::clamp(want, 0, 110);
    {
        auto *v = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(opt) + 0x80);
        auto *ve = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(opt) + 0x88);
        size_t nvals = (v && ve > v && size_t(ve - v) % 0x50 == 0) ? size_t(ve - v) / 0x50 : 0;
        if (nvals > 1 && nvals < 111) {
            // A short value list (e.g. preset levels): map 0..110 onto it rather than setting an index past its end.
            int mapped = static_cast<int>(std::lround(want * double(nvals - 1) / 110.0));
            ilog("[instant-race] AI difficulty: option has %zu values; slider %d maps to index %d", nvals, want, mapped);
            want = mapped;
        }
    }
    if (!ai_set_index(opt, want)) { ilog("[instant-race] AI difficulty: option has no setter"); return; }
    if (!g_ai_option) { g_ai_option = opt; g_ai_saved_index = before; }
    ilog("[instant-race] AI difficulty: %d -> %d (now reads %d)", before, want, ai_get_index(opt));
}
void restore_ai_difficulty() {
    if (!g_ai_option || g_ai_saved_index < 0) return;
    ai_set_index(g_ai_option, g_ai_saved_index);
    ilog("[instant-race] AI difficulty put back to the player's %d", g_ai_saved_index);
    g_ai_option = nullptr;
    g_ai_saved_index = -1;
}

void on_session_start(const SwoftyEvent *, void *) {
    g_in_session.store(true);
    g_menu_open = false;
}

void on_session_end(const SwoftyEvent *, void *) {
    restore_ai_difficulty();
    g_stage.store(static_cast<int>(LaunchStage::idle));
    g_load_sync_done = false;
    g_fade_released = false;
    g_scene_despawned = false;
    g_load_exit_ts = 0;
    g_load_sync_tries = 0;
    g_chose_sess.store(false);
    g_in_session.store(false);
}

uint32_t toggle_vk() {
    std::string_view k = menu_key;
    if (k == "F7") return VK_F7;
    if (k == "F8") return VK_F8;
    if (k == "F10") return VK_F10;
    if (k == "Insert") return VK_INSERT;
    if (k == "Home") return VK_HOME;
    return VK_F7;
}

void trigger_launch() {
    resolve_all_game_addrs();
    if (!g_flow_hook_installed || !g_addrs.scanned) {
        ilog("[instant-race] Launch refused: launcher bindings missing (%s)",
             g_flow_hook_installed ? g_missing_bindings.c_str() : "site_FeFlow_AfterStateLink");
        swofty::overlay::ui::toast(g_ui_state, "Instant Race needs a newer Swofty Launcher: update it and restart the game.");
        return;
    }
    if (g_raced_once) ilog("[instant-race] Another Instant Race in this game session");
    {
        int n = ++g_launch_no;
        g_bss_calls.store(0);
        g_bss_race.store(0);
        g_bss_idle_logged.store(0);
        g_relaid.store(false);
        g_grid_snaps = 0;
        g_game_laps.store(0);
        g_fuel_logs.store(0);
        ilog("[instant-race] grid: launch #%d of this game session", n);
    }
    if (g_cfg.surprise) randomize_all();
    sync_distance_laps();
    if (g_cfg.distance > 0)
        ilog("[instant-race] Race distance: %s at %s = %.0f laps%s", kDistances[g_cfg.distance].label, g_cfg.track_id.c_str(),
             g_cfg.laps, real_race_laps(g_cfg.track_id) ? "" : " (add-on track: no real F1 race, full distance taken as 60 laps)");
    g_menu_open = false;
    swofty::overlay::focus(false);
    g_was_focused = false;
    g_load_sync_done = false;
    g_fade_released = false;
    g_scene_despawned = false;
    g_load_exit_ts = 0;
    g_load_sync_tries = 0;
    g_chose_sess.store(false);
    g_session_at_ms.store(0);
    g_session_visible.store(false);
    g_last_fade_mask = ~0u;
    g_last_fade_state = ~0u;
    g_fade_log_count = 0;
    ilog("[instant-race] v1.0.5 Launch triggered: era=%s track=%s team=%s driver=%s laps=%.0f weather=%s time=%s surprise=%d", g_cfg.era_id.c_str(), g_cfg.track_id.c_str(), g_cfg.team_id.c_str(), g_cfg.driver_id.c_str(), g_cfg.laps, g_cfg.weather.c_str(), time_of_day_label(g_cfg.time_of_day), g_cfg.surprise ? 1 : 0);
    g_stage.store(static_cast<int>(LaunchStage::set_mode));
    swofty::overlay::ui::toast(g_ui_state, "Launching Instant Race at " + g_cfg.track_label + "...");
}

bool is_main_menu_ready() {
    if (g_in_session.load()) return false;
    uint64_t seen = g_frontend_seen_ms.load();
    return seen != 0 && (GetTickCount64() - seen) < 500;
}

void on_paint_overlay(swofty::overlay::Paint *paint, void *) {
    using namespace swofty::overlay;
    auto in = input(paint);

    bool on_home = is_main_menu_ready();

    if (on_home && !g_initialized_roll && swofty::db::available()) {
        std::lock_guard lock(g_mutex);
        randomize_all();
        g_initialized_roll = true;
    }

    if (on_home && in.key_pressed(toggle_vk())) {
        g_menu_open = !g_menu_open;
        if (g_menu_open && !g_initialized_roll) {
            std::lock_guard lock(g_mutex);
            randomize_all();
            g_initialized_roll = true;
        }
    }

    if (g_menu_open != g_was_focused) {
        focus(g_menu_open);
        g_was_focused = g_menu_open;
    }

    ui::Ui u(paint, g_ui_state);

    uint64_t sat = g_session_at_ms.load();
    int cur_launch_st = g_stage.load();
    bool launching_now = (cur_launch_st != static_cast<int>(LaunchStage::idle) && cur_launch_st != static_cast<int>(LaunchStage::done));
    if (launching_now || (sat && !g_session_visible.load() && (GetTickCount64() - sat) < 60000)) {
        std::lock_guard lock(g_mutex);
        float sw = 0, sh = 0;
        size(paint, &sw, &sh);
        rect(paint, 0, 0, sw, sh, 0x0B0D12FF);
        // Tracks outside the F1 25 calendar come from add-ons, and the first load of one can take several minutes
        // (Laguna Seca about 7); say so, or it looks frozen.
        const bool slow_track = !g_cfg.track_id.empty() && real_race_laps(g_cfg.track_id) == 0;
        const float card_h = slow_track ? 270.0f : 210.0f;
        Rect card{(sw - u.u(640.0f)) * 0.5f, (sh - u.u(card_h)) * 0.5f, u.u(640.0f), u.u(card_h)};
        Rect body = ui::panel(u, card, "INSTANT RACE  -  LOADING GRAND PRIX");
        char line1[256], line2[256], line3[256];
        bool hide = g_cfg.surprise;
        std::snprintf(line1, sizeof(line1), "Season / Era:   %s", hide ? "???" : g_cfg.era_label.c_str());
        if (hide) std::snprintf(line2, sizeof(line2), "Car & Driver:   ???  -  Surprise!");
        else std::snprintf(line2, sizeof(line2), "Car & Driver:   %s  -  %s", g_cfg.team_label.c_str(), g_cfg.driver_label.c_str());
        std::snprintf(line3, sizeof(line3), "Circuit & Race: %s  |  %.0f Laps  |  %s  |  %s",
                      hide ? "???" : g_cfg.track_label.c_str(), g_cfg.laps, hide ? "???" : g_cfg.weather.c_str(),
                      hide ? "???" : time_of_day_label(g_cfg.time_of_day));
        float ly = body.y + u.u(6.0f);
        ui::label(u, {body.x, ly, body.w, u.u(26.0f)}, line1, u.th.text); ly += u.u(30.0f);
        ui::label(u, {body.x, ly, body.w, u.u(26.0f)}, line2, u.th.text); ly += u.u(30.0f);
        ui::label(u, {body.x, ly, body.w, u.u(26.0f)}, line3, u.th.text); ly += u.u(36.0f);
        ui::label(u, {body.x, ly, body.w, u.u(24.0f)},
                  sat ? "Lining up on the grid (broadcast intro)..." : "Building custom Grand Prix weekend...", u.th.dim);
        if (slow_track && !sat) {
            ly += u.u(30.0f);
            ui::label(u, {body.x, ly, body.w, u.u(24.0f)},
                      hide ? "This one can take a few minutes to load. Leave it running."
                           : "Add-on track: loading can take a few minutes. Leave it running.",
                      u.th.text);
        }
        return;
    }

    if (show_menu_prompt && on_home && !g_menu_open) {
        float sw = 0, sh = 0;
        size(paint, &sw, &sh);
        // The game draws its menus in a centred 16:9 area; on ultrawide and triple screens anchor the prompt there
        // instead of at the far right of the whole display.
        float area_x = 0, area_w = sw;
        if (sh > 0 && sw / sh > 16.0f / 9.0f + 0.01f) {
            area_w = sh * 16.0f / 9.0f;
            area_x = (sw - area_w) * 0.5f;
        }
        Rect pill{area_x + area_w - u.u(275.0f), u.u(138.0f), u.u(215.0f), u.u(38.0f)};
        if (ui::button(u, pill, "[F7] Instant Race")) {
            g_menu_open = true;
            if (!g_initialized_roll) {
                std::lock_guard lock(g_mutex);
                randomize_all();
                g_initialized_roll = true;
            }
            focus(true);
            g_was_focused = true;
        }
    }

    if (!g_menu_open) {
        ui::draw_toasts(u);
        return;
    }

    if (u.back) {
        g_menu_open = false;
        focus(false);
        g_was_focused = false;
        return;
    }

    float sw = 0, sh = 0;
    size(paint, &sw, &sh);
    float pw = std::min(sw * 0.78f, u.u(940.0f));
    float ph = std::min(sh * 0.86f, u.u(680.0f));
    Rect win{(sw - pw) * 0.5f, (sh - ph) * 0.5f, pw, ph};

    rect(paint, 0, 0, sw, sh, 0x00000095);
    Rect body = ui::panel(u, win, "INSTANT RACE  -  RANDOM GRAND PRIX GENERATOR");

    std::lock_guard lock(g_mutex);

    float y = body.y + u.u(6.0f);
    float h = u.u(40.0f);
    float gap = u.u(12.0f);
    float reroll_w = u.u(150.0f);
    float main_w = body.w - reroll_w - gap;

    ui::label(u, {body.x, y, body.w - u.u(130.0f), u.u(26)},
              "Rolled from your installed seasons, teams, drivers & tracks:", u.th.dim);
    if (ui::button(u, {body.x + body.w - u.u(120.0f), y, u.u(120.0f), u.u(28)}, "Close [F7]")) {
        g_menu_open = false;
        focus(false);
        g_was_focused = false;
        return;
    }
    y += u.u(34.0f);

    char buf[256];

    if (g_cfg.surprise) {
        ui::label(u, {body.x, y, body.w, h}, "SURPRISE MODE IS ON", u.th.text);
        y += h + gap;
        ui::label(u, {body.x, y, body.w, h}, "Era, car & driver, circuit, weather and time of day are picked at random when you press Start.", u.th.dim);
        y += h + gap;
        ui::label(u, {body.x, y, body.w, h}, "You'll find out what you got when the race loads. Your race length below still applies.", u.th.dim);
        y += (h + gap) * 2;
    } else {
    // 1. Season / Era
    std::snprintf(buf, sizeof(buf), "Season / Era:   %s", g_cfg.era_label.c_str());
    if (ui::button(u, {body.x, y, main_w, h}, buf)) {
        auto eras = get_installed_eras();
        if (!eras.empty()) {
            size_t idx = 0;
            for (size_t i = 0; i < eras.size(); ++i)
                if (eras[i].id == g_cfg.era_id) { idx = (i + 1) % eras.size(); break; }
            g_cfg.era_id = eras[idx].id;
            g_cfg.era_label = eras[idx].label;
            g_cfg.era_row_index = eras[idx].row_index;
            reroll_seat_for_current_era();
        }
    }
    if (ui::button(u, {body.x + main_w + gap, y, reroll_w, h}, "Reroll Era")) {
        reroll_era();
    }
    y += h + gap;

    // 2. Team & Driver
    std::snprintf(buf, sizeof(buf), "Car & Driver:   %s  -  %s", g_cfg.team_label.c_str(), g_cfg.driver_label.c_str());
    if (ui::button(u, {body.x, y, main_w, h}, buf)) {
        auto seats = get_seats_for_era(g_cfg.era_id, g_cfg.era_row_index);
        if (!seats.empty()) {
            size_t idx = 0;
            for (size_t i = 0; i < seats.size(); ++i)
                if (seats[i].team_id == g_cfg.team_id && seats[i].driver_id == g_cfg.driver_id) {
                    idx = (i + 1) % seats.size();
                    break;
                }
            g_cfg.team_id = seats[idx].team_id;
            g_cfg.team_label = seats[idx].team_label;
            g_cfg.driver_id = seats[idx].driver_id;
            g_cfg.driver_label = seats[idx].driver_label;
        }
    }
    if (ui::button(u, {body.x + main_w + gap, y, reroll_w, h}, "Reroll Seat")) {
        reroll_seat_for_current_era();
    }
    y += h + gap;

    // 3. Track
    std::snprintf(buf, sizeof(buf), "Circuit:        %s", g_cfg.track_label.c_str());
    if (ui::button(u, {body.x, y, main_w, h}, buf)) {
        auto tracks = get_installed_tracks();
        if (!tracks.empty()) {
            size_t idx = 0;
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i].id == g_cfg.track_id) { idx = (i + 1) % tracks.size(); break; }
            g_cfg.track_id = tracks[idx].id;
            g_cfg.track_label = tracks[idx].label;
        }
    }
    if (ui::button(u, {body.x + main_w + gap, y, reroll_w, h}, "Reroll Track")) {
        reroll_track();
    }
    y += h + gap;

    // 4. Weather
    std::snprintf(buf, sizeof(buf), "Weather:        %s", g_cfg.weather.c_str());
    if (ui::button(u, {body.x, y, main_w, h}, buf)) {
        g_cfg.weather = next_weather(g_cfg.weather);
    }
    if (ui::button(u, {body.x + main_w + gap, y, reroll_w, h}, "Reroll Weather")) {
        std::uniform_int_distribution<int> wpick(0, kWeathersCount - 1);
        std::string was = g_cfg.weather;
        while (g_cfg.weather == was) g_cfg.weather = kWeathers[wpick(g_rng)];
    }
    y += h + gap;

    // 5. Time of day
    std::snprintf(buf, sizeof(buf), "Time of Day:    %s%s", time_of_day_label(g_cfg.time_of_day),
                  kTimesOfDay[g_cfg.time_of_day].preset == 7 ? "  (floodlit tracks; sunset elsewhere)" : "");
    if (ui::button(u, {body.x, y, main_w, h}, buf)) {
        g_cfg.time_of_day = (g_cfg.time_of_day + 1) % kTimesOfDayCount;
    }
    if (ui::button(u, {body.x + main_w + gap, y, reroll_w, h}, "Reroll Time")) {
        std::uniform_int_distribution<int> tdist(1, kTimesOfDayCount - 1);
        g_cfg.time_of_day = tdist(g_rng);
    }
    y += h + gap;

    }

    // 5. Race Distance (1-100 laps, or a share of the track's real race) & AI Difficulty
    float half_w = (body.w - gap) * 0.5f;
    float slider_h = u.u(48.0f);
    // In Surprise mode the track isn't known until Start, so a preset shows its name instead of a lap count.
    bool preset_hidden = g_cfg.surprise && g_cfg.distance > 0;
    if (!preset_hidden) sync_distance_laps();
    char dist_fmt[64];
    if (preset_hidden)
        std::snprintf(dist_fmt, sizeof(dist_fmt), "%s (set at Start)", kDistances[g_cfg.distance].label);
    else if (g_cfg.distance > 0)
        std::snprintf(dist_fmt, sizeof(dist_fmt), "%%.0f Laps (%s)", kDistances[g_cfg.distance].label);
    else
        std::snprintf(dist_fmt, sizeof(dist_fmt), "%%.0f Laps");
    if (ui::slider(u, {body.x, y, half_w, slider_h}, "Race Distance", g_cfg.laps, 1.0f, 100.0f, dist_fmt)) {
        g_cfg.laps = std::round(g_cfg.laps);
        g_cfg.distance = 0; // moving the slider means a custom lap count
    }
    // The game's AI difficulty isn't set by the mod yet (its storage hasn't been found); races use the player's own
    // Grand Prix setting, so say so instead of showing a slider that does nothing.
    ui::label(u, {body.x + half_w + gap, y, half_w, slider_h * 0.5f}, "AI Difficulty: your Grand Prix setting", u.th.text);
    ui::label(u, {body.x + half_w + gap, y + slider_h * 0.5f, half_w, slider_h * 0.5f},
              "Change it in Grand Prix > Settings > Difficulty", u.th.dim);
    y += slider_h + gap * 0.6f;
    {
        float bw = (half_w - gap * 2) / 3.0f;
        for (int d = 1; d < kDistancesCount; ++d) {
            char lbl[48];
            std::snprintf(lbl, sizeof(lbl), g_cfg.distance == d ? "> %s <" : "%s", kDistances[d].label);
            if (ui::button(u, {body.x + (bw + gap) * float(d - 1), y, bw, u.u(36.0f)}, lbl)) {
                g_cfg.distance = g_cfg.distance == d ? 0 : d; // pressing the chosen one again goes back to the slider
                sync_distance_laps();
            }
        }
        y += u.u(36.0f) + gap * 1.3f;
    }

    // 6. Roll All & Launch Buttons
    float roll_w = u.u(230.0f);
    if (ui::button(u, {body.x, y, roll_w, u.u(50.0f)}, "Roll All Random")) {
        randomize_all();
        ui::toast(g_ui_state, "Rolled a brand new random Grand Prix!");
    }
    if (ui::button(u, {body.x + roll_w + gap, y, roll_w, u.u(50.0f)}, g_cfg.surprise ? "Surprise: ON" : "Surprise: OFF")) {
        g_cfg.surprise = !g_cfg.surprise;
        ui::toast(g_ui_state, g_cfg.surprise ? "Surprise mode on: everything but race length is a mystery." : "Surprise mode off.");
    }
    if (ui::button(u, {body.x + (roll_w + gap) * 2, y, body.w - (roll_w + gap) * 2, u.u(50.0f)}, "START INSTANT RACE")) {
        trigger_launch();
    }

    ui::draw_toasts(u);
}

bool init_plugin() {
    if (!swofty::api || !swofty::api->resolve) return true;

    for (const char *sym : {"Game_ChampionshipGameMode_BuildSessionSettings", "game.championship_game_mode_build_session_settings"}) {
        if (swofty::api->resolve(sym) != 0) {
            swofty::detail::append(swofty::detail::mixins,
                new swofty::detail::node{
                    {sym, SWOFTY_MIXIN_WRAP, 0,
                     reinterpret_cast<void *>(&hook_build_settings),
                     reinterpret_cast<void **>(&orig_build_settings)},
                    nullptr});
            break;
        }
    }

    if (swofty::api->resolve("Game_CarSetup_SessionFuelLoad") != 0) {
        swofty::detail::append(swofty::detail::mixins,
            new swofty::detail::node{
                {"Game_CarSetup_SessionFuelLoad", SWOFTY_MIXIN_WRAP, 0, reinterpret_cast<void *>(&hook_fuel_load),
                 reinterpret_cast<void **>(&orig_fuel_load)},
                nullptr});
    } else {
        swofty::log("[instant-race] Game_CarSetup_SessionFuelLoad is not in this Swofty Launcher: the race keeps the game's fuel load");
    }

    // The flow site carries the whole launch (leaving the menu, the load sync, returning to the menu). Without it
    // Instant Race stays off; registering a mixin on a binding the launcher lacks would drop the addon.
    if (swofty::api->resolve("site_FeFlow_AfterStateLink") != 0) {
        swofty::detail::append(swofty::detail::mixins,
            new swofty::detail::node{
                {"site_FeFlow_AfterStateLink", SWOFTY_MIXIN_SITE, 0, reinterpret_cast<void *>(&on_flow_site), nullptr},
                nullptr});
        g_flow_hook_installed = true;
    } else {
        swofty::log("[instant-race] site_FeFlow_AfterStateLink is not in this Swofty Launcher; update the launcher to use Instant Race");
    }

    g_tod_build = reinterpret_cast<TodBuildFn *>(swofty::api->resolve("Sess_Tod_BuildSessionDataset_Thunk"));
    if (!g_tod_build) swofty::log("[instant-race] Sess_Tod_BuildSessionDataset_Thunk not found: changing weather falls back to its first state");
    if (swofty::api->resolve("Sess_GameWeatherSystem_SetupSession") != 0) {
        swofty::detail::append(swofty::detail::mixins,
            new swofty::detail::node{
                {"Sess_GameWeatherSystem_SetupSession", SWOFTY_MIXIN_WRAP, 0,
                 reinterpret_cast<void *>(&hook_weather_setup),
                 reinterpret_cast<void **>(&orig_weather_setup)},
                nullptr});
    }

    return true;
}

} // namespace

SWOFTY_ON_LOAD {
    swofty::events::subscribe(SWOFTY_EVENT_SESSION_START, on_session_start);
    swofty::events::subscribe(SWOFTY_EVENT_SESSION_END, on_session_end);
    swofty::events::on_frame(&on_flow_frame);
    if (swofty::overlay::available()) {
        swofty::overlay::on_paint(&on_paint_overlay);
    }
    swofty::log("[instant-race] v1.0.5 loaded! Press F7 on the main menu to open Instant Race.");
}

SWOFTY_ADDON(init_plugin)

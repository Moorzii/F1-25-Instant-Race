# Instant Race roadmap

Planned work, roughly in order. Notes under each item are where to start.

## 0. Ghost grid on the first load (intermittent)
On some loads every car spawns on the start/finish line instead of its grid slot; restarting the session always fixes it.
- Log signal: halo-re's lap distance for the player equals the full track length (5810 m Suzuka, 7007 m Spa, 5409 m
  Bahrain) on every ghosted run; a normal grid slot on every good run. 10 ghosted and 15 good runs this week.
- No difference found in anything logged before placement: profile, version, sessions present, track, team, seat,
  laps, weather, time since start-up, menus visited, load timing, Instant Race's own steps (identical setups at 00:27
  and 01:00 on 19 Nov gave one bad and one good run). Only after-effects differ (lap event at spawn, no grid player shot).
- Tried and ruled out: race session state (+0x10 = 1), the grid builders (SetupStartingGrid / DriverBasedGrid output is
  empty on good and bad loads alike), rebuilding the weekend.
- Lead from Swofty's direct start: it holds the interrupt exit so the menu's main scene (and its PhysicsTerrain) is
  gone before the track's is created. If the menu scene is sometimes still there when the cars are placed, placement
  could fall back to the line, and timing would make it intermittent. The fix2 test build logs the menu scene's
  spawned flags (StateFrontend +0xd20 -> +0x40 -> +0x1048/+0x1049) during the load and at grid placement.
- Placement logging exists (Game_VehicleProgressSystem_OnVehiclePlacedOnGrid fires for all cars at the end of loading;
  Game_GridSlot_PlaceVehicle is not used for a Grand Prix). 4 good runs recorded on 20 Nov, no ghosted one yet: the
  next ghosted run with that logging is the comparison needed.

- New lead (Send Logs, 21 Nov, Racingfox2309, 1.0.4): both bad grids they reported were later races in the session
  that took the relayout path ("The weekend had no sessions after the build; laid out again"): Spa 2016 at 02:10
  (grid out of order) and Zandvoort reverse 2023 at 07:53 (empty "Starting Grid" screen, AI with instant lap times).
  Their other relayout races: Long Beach 2016 (09:06; the game then hung in StateUnloadSession on the way back to the
  menu, load-stall report at 09:13), Austria reverse 2018 (09:46, outcome unknown), Laguna Seca 2022 (10:07; crashed
  at F1_25.exe+0x6599b0, reading 0x128, 16 ms after StateLoadSession exited, flow state machine on the stack). None of
  their races without relayout was reported bad. Brad's own ghosted runs (1.0.3, first loads) did not take this
  path, so there may be two causes. Next: find why the weekend loses its sessions on some later launches (era or track
  change doesn't explain it), and what Sess_WeekendSetup_LayoutWeekends resets besides the sessions (grid order?).
- Second lead (bot's Send Logs bundle case-C, 22 Nov, 1.0.4): in that player's session only the first Instant Race
  logged "Applied custom race length"; the later ones (Austria reverse with the relayout, Imola without it, Laguna
  Seca) never reached the Game_ChampionshipGameMode_BuildSessionSettings wrap, so those races kept the previous
  session's settings. Brad's own later races always hit the wrap. Check whether the wrap firing (or not) lines up with
  the bad grids, and whether rebuilding the session settings ourselves on later launches fixes them.
- Swofty 1.10.0 (20 Nov) skips the game's 3 s minimum loading screen. If the ghost grid is a timing race with the menu
  scene, it may now show up more or less often: note the launcher version with every ghosted report.

## 1. Time of day (done on `next`, for 1.0.5)
Race start time: Track Default, Sunrise, Morning, Midday, Afternoon, Sunset, Night; also in Roll All and Surprise.
- Set in the weather hook: WeatherSessionSettings +0xb10/+0xb11 cleared, +0xb19 and +0xb28 + d*8 = preset.
  GameWeatherSystem::setupSession (0x1438b98e0 on this build) maps the preset: 0 = A-1h, 1 = A, 2 = (A+midday)/2,
  3 = midday, 4 = (midday+B)/2, 5 = B, 6 = B+1h, 7 = 00:00, where A/B are the track's two preset times (track weather
  config +0x10/+0x18, e.g. Silverstone 4.75/19.25) and midday is from the solar calc. Result at +0x1ec8/+0x1ecc.
- Tested 20 Nov: Sunset works at Silverstone; Night (00:00) works at Losail (floodlights) but shows sunset at
  Silverstone. Track weather config +0x29d is the night-lighting flag (1 at Losail, 0 at Silverstone), so Night falls
  back to Sunset where it is 0. Preset 6 (B+1h) dropped: at Silverstone it lands on the real sunset (20:05).
- Checked 20 Nov (tod3): Afternoon at Singapore starts 14:43 and looks right; Night at Silverstone falls back to Sunset (19:15).

## 1b. Changing weather (parked, code on `next` but not offered in the menu)
Dry to Wet / Wet to Dry. Two routes tried, both give an all-white screen on the grid (Bahrain, 20 Nov):
- wx1: scripted slots through setupSession's exact-timing path (+0xb18). Bug: trans[i] is the change OUT of state i
  (into i + 1), not into it, so the first change was 0 s.
- wx2/wx3: the game builds the first state, then each dataset is rebuilt with Sess_Tod_BuildSessionDataset_Thunk the
  way Swofty's weather chain does (hold/trans >= 1 s, last state to the end, rain-range check, finite check). The
  rebuilt race dataset looked right (clear / overcast / light rain 0.33 / heavy rain 0.66, len 1800) and still went
  white, even with Track Default time. Bahrain Afternoon Dry alone is fine. Swofty ships its chain off by default
  (SWOFTY_WEATHER_CHAIN=1), possibly for the same reason.
- Next idea: the game's own Grand Prix custom weather, i.e. the non-exact custom-slot path (+0xb14 = 0, count > 1,
  types per state, no hold/trans) that setupSession builds at 0x1438ba590; it takes the dataset length from the slot
  (+0x00) or per-session constants, so the race slot needs a sensible length (the game's race dataset was 1800 s).
- Release plan: 1.0.5 (time of day) is on hold for now.

## 2. More season packs in the era list
Support the GP/TT packs (e.g. "F1 2015 GP/TT Addon") and Classic Seasons, not only Swofty's own `sw_f1_<year>` grids.
- Need one player log with those packs installed to see their entrant list names.
- Then extend `get_installed_eras()` to accept those list names, and the build to resolve them.

## 3. AI difficulty slider
Bring the slider back once the game's 0-110 setting is found (Swofty's docs list it as unknown too).
- Plan: a memory search in the mod. Snapshot every int32 equal to the player's current Grand Prix AI level, change it in the Grand Prix menu, keep the addresses that changed to the new value. Menu-only tests, no races.
- Not in the options store under `ai_difficulty`, `ai_level`, `difficulty` or `aiDifficulty`, and not in the session settings dump.

## 4. Black screen on the next career / Grand Prix / Instant Race after an Instant Race
Root cause found (20 Nov, diagnostic builds with per-state link logging):
- Normal career start: StateFrontend (957880c6) leaves by its exit link, then StateFrontendWorld (290b11ae) reports
  the session link (8c23ce89) and the game opens the loading states under e4ef00ba.
- After an Instant Race: StateFrontendWorld reports its exit link (0eef86be) instead, so e4ef00ba, 40b45c37 and
  e39dcbd5 exit in turn and the whole game flow unwinds to the root (black screen).
- StateFrontendWorld already reported that exit link when Instant Race launched: the launch raised eInterruptExit
  (0xe) on the shared interrupt queue. The queue type itself is back to 0 before the race (the game consumes it), but
  the menu's world state keeps leaving by its exit link afterwards.
- Not the cause (tested): the queue flag being left set (fix1 cleared it on the exact queue; no change).
- Not the fix (tested 20 Nov, 03:20): fix2, which stopped raising eInterruptExit. StateFrontend went to status 3 and
  the menu scene despawned, but its exit never finished and no fade was requested; the launch timed out after 60 s on a
  black main menu. Instant Race's path needs the interrupt for StateFrontend to finish exiting.
- Cause found (diag4, 20 Nov 03:50): StateFrontendWorld::onStateUpdate (0x1436d94e0) returns eLinkExit whenever its
  byte +0xdc0 is set; the game's own TEMP_LOG strings ("FINISHED LINK:eLinkExit", ":session", ":story", ...) name each
  branch. The exit interrupt sets that byte, and nothing clears it, since a menu exit normally quits the game. Instant
  Race turns that exit into a race, so the byte stays set and every later menu exit leaves by eLinkExit. The object
  itself (0x0..0xd40) was identical at start-up and after the race apart from the child pointer and timers.
- **Fixed (fix3, tested 20 Nov 20:14):** clearing +0xdc0 when Instant Race sends StateFrontendWorld to the session
  link. Log: flag was 1 at launch, cleared; after the race StateFrontendWorld reported the session link (8c23ce89,
  flag 0) and career loaded.
- fix3b (20 Nov 20:39, second-launch block removed): the first race and career were fine, but the second Instant Race
  hung like fix2. StateFrontend went to status 3, but no fade was requested, and it never finished exiting.
- Cause: StateFrontend::onStateInterrupt (0x1436d70a0) handles eInterruptExit (0xe, jump table at 0x14587644c) by
  requesting the "ClosingGame" fade-out (fade mask bit 0x80000000, the bit Instant Race clears by hand) only when bit
  0x08 of StateFrontend+0xf90 is clear, then sets that bit. Nothing clears it, so the second exit gets no fade.
- **Fixed (fix3c, tested 20 Nov 20:48):** two Instant Races in one game session, then career, all worked; both
  latches logged as set (+0xdc0 = 1, +0xf90 = 0x0a) and were cleared at each launch. In 1.0.4 as
  clear_menu_exit_latches(), and the second-launch block is removed.
- The menu fade bit 0x80000000 that Instant Race clears by hand is the "ClosingGame" fade reason (see above). 
- Swofty's own direct start (launcher 1.9.91, native/src/core/direct_start.cpp) raises the same interrupt only on
  StateFTUE as it leaves, and clears it the moment StateFTUE's onStateExit finishes, so its parents never see it.
  Instant Race raised it on the first state its hook saw (e4ef00ba, a parent) and kept it set for ~3 s until
  StateFrontendWorld had no child, long enough for the parents to latch it.

## 4a. Crash on loading: no track data set for the list's season (fix on `next`, tested with td2/td3, for 1.0.6)

Report 23 Nov (launcher 1.10.30): two launches crashed the same way on the loading screen, null read of +0x268 at
F1_25.exe+0x2508873, inside BuildSessionSettings (called from our wrap). `Game_TrackData_GetTrack` (1.0.145 0x2e63390)
returned 0: the query was mode 20 (swofty_f1), season 8 (season_2026, from the entrant list's `season` field, list
+0xb0/+0xb1), format 0, and no `TrackDataSet` lists swofty_f1 with season_2026 (swofty_f1 is only in the f1_2023,
f1_2024 and f1_2025 sets). So any list that names a season its setup mode has no data set for crashes, whatever the
track; the two reports were on add-on tracks by chance. Fix: before the build, walk the track system's data sets
(+0x68..+0x70, 0x30 rows, masks +0x20/+0x24/+0x28) like the game does; when none matches, run the list as a season
the mode has a set for (see td2 below); refuse the launch with a toast if even that
finds nothing. Checked again in the BuildSessionSettings wrap. The test copy can force a list's season to 8 with
`%LOCALAPPDATA%\Swofty\instant_race_td_test.txt` to reproduce on any season.
td1 (season left out of the query) crashed in Game_GameMode_AddTrackLayerWorlds (+0x7a143d, writing far past its
32-row stack list): the same query picks the extra layer worlds, and with no season every season's layers matched.
td2 instead sets the list's season to one the mode has a data set for (lowest season of the last set listing the mode
and the default format: season_2025, stat 7, for swofty_f1).

## 4c. Fuel too low for the race (fuel1 test)

23 Nov: Short distance at Zandvoort reverse (18 laps), the engineer warned of low fuel after 4 laps; restarting the
race gives no setup screen (race-only weekend, parc ferme). The fuel load comes from the game's own Grand Prix race
length option (weekend_options.race_length), not from the lap count written into SessionSettings +0xdf0 after the
build. fuel1 logs the game's own lap count next to ours and sets the race length option (found by name through
Game_OptionsStore_FindOption / g_Game_OptionsStore, index get/set through option vtable slots 2/3) to the smallest
percent value covering the race before the weekend is built, putting the player's value back at session end.
fuel1 result: the game's build had 5 laps (option race_length index 3 = 5_laps), but the option setter (vtable slot 3)
did not take the index (still read 3 after the set), and lap 1 showed -10 laps of fuel. fuel2 instead sets the
weekend's race length byte that Game_CarSetup_SessionFuelLoad reads (+0x28 of the entry in weekend +0x258..+0x260,
same index order as the option) at build and again at StateLoadSession, and logs the list bytes to confirm the layout.
fuel2 result: the byte was 3 and took 4 (the list holds a 16-byte pointer pair), but the build still had 5 laps and
lap 1 still showed -10.37 laps of fuel, so that was not the source; removed. fuel3 wraps Game_CarSetup_SessionFuelLoad
(guarded by resolve) and scales its result by (our laps + 2) / (game laps + 2), capped at 110 kg.
fuel3 result: Game_CarSetup_SessionFuelLoad is not an exported binding ("bindings.steam-26566308.json does not
list"), so the wrap was skipped; removed. Root cause found: our own weekend plan call
(Sess_Build_DevWeekendStructure(plan, gsm, 0, 0, race, 0, 1, 0, 0)) passed race = 3, which is the race length index
5_laps, so the game built and fuelled every race for 5 laps. fuel4 passes the smallest percent index covering the
race (4 = 25%, 5 = 35%, 6 = 50%, 7 = 100%); the exact lap count is still written after the build.

Ghost grid reproduced 23 Nov 19:46 (fuel3, Zandvoort): after Brad opened the game's Grand Prix menu, the launch logged
"The weekend had no sessions after the build; laid out again" and every car stood at the same spot (lap 0, 4259 m,
same x/z) at 5 s and 20 s. In the 9 earlier launches (no relayout) every grid was correct. So the ghost grid is the
relayout path (Sess_WeekendSetup_LayoutWeekends after the season build), reached when the Grand Prix menu was visited.
fuel4 (19:53, 19:55): fuel fixed (+2.62 laps, the game's build had 18). Both launches took the relayout path but the
grid slots were right (same positions as a normal Zandvoort grid: P1 at 58-60 m past the timing line). Before the
second, Brad deleted the Instant Race Grand Prix save slot in the Grand Prix menu (as before the ghost grid): that race
had the phantom lap (lap 2/18 at the start), fastest-lap spam, and the timing order wrong (started near the front,
shown P17). Lead: no current Grand Prix save slot. slot1 logs the save slot manager (slots +0x268..+0x270, current
+0x280) at each launch and grid snapshots at 5/20/45/75 s, to compare a launch with and without the slot.
slot1 results (21:01-21:21): the save slot theory is wrong: after each delete the game makes a new current slot
(#5 -> #6 -> #7, 'F1 25 Save 1'). What the broken launches share is the relayout path ("no sessions after the build;
laid out again"): A relayout good, B relayout (broadcast grid screen empty, the game's build had 5 laps so the fuel was
short at Bahrain, -6.43 laps), C no relayout good, D relayout (phantom lap: at 75 s the back-of-grid cars, which start
before the timing line, were already on lap 1). Every launch without relayout today was good. The relayout calls
Sess_WeekendSetup_LayoutWeekends directly; the game reaches it through the WeekendSetupComponent's vtable slot +0x290,
whose body calls LayoutWeekends first and then does more. layout1 calls slot +0x290 instead (logging the vtable), to
test whether the skipped rest is what sets up the grid screen, the plan's race length and the start line state.
layout1 results (21:34-21:44): slot +0x290 (vtable game+0x567fc60, slot 0x140b797f0) made no difference. Relayout
launches: no starting grid at all, the grid order is the vehicle order v0, v1, v2... (a normal launch is shuffled, e.g.
v16, v3, v10), all cars on one spot at Bahrain (with DSQ messages), an empty grid screen, and the phantom lap at
Montreal (every car lap 1 at 75 s, while a normal launch is still lap 0 at 295 m). The relayout path never makes a
starting grid, and four patches to it failed, so stop patching it. rebuild1: when the weekend comes back empty, run
the whole build (plan, season build, weekend structure, entrants, seat) a second time and log the session count per
pass; the old relayout stays only as a last resort.
rebuild1 result (22:00, fresh save, Spa): the second build also left the weekend empty (pass 1: 0 sessions, pass 2: 0),
so it fell back to the relayout and the race was broken again (empty grid screen, grid in vehicle order, fastest-lap
spam, lap 2/5). Reverted. Five attempts at this path have failed: next step is asking the Swofty Manager bot which
function makes the starting grid / race start state for a new Grand Prix weekend, and what the game's own Grand Prix
start does on a new save that Game_CustomSeason_RebuildFromRounds does not.
Swofty bot answer (23 Nov 23:07): the starting grid is made by GridSetupComponent::setupStartingGrid
(Game_GridSetup_SetupStartingGrid 0x141210160, helpers Game_GridSetup_DriverBasedGrid, Game_GridSetup_FindQualiResult
"Couldn't find qualifying result for driver %s"), the championship's Game_EventListGridSetupComponent_SetupStartingGrid
0x140b9cd70 (GridOrderOverride) which calls it, and Game_FeaturedEventModeComponent_SetupStartingGrid 0x141207300,
whose vtable cell 0x1456c9f60 sits next to the cell holding Game_CustomSeason_RebuildFromRounds (0x1456c9f58).
Placement: Game_VehiclePlacementSystem_HandleEncodedGridMessage, Game_Read_GridConfig, Game_GridSlot_PlaceVehicle,
Sess_VehiclePlacement_PlaceOnSlot, per-car grid position SessionVehicleEntry +0x90 (xor, position - 1). Start state:
Sess_PreRaceGridState_OnEnter, g_Game_RaceStartSystem, Game_ChampionshipFormationLapComponent_OnRaceStarted (not
exported). Not known: who calls setupStartingGrid and whether it runs on a fresh save. grid2 (test only) wraps the three
setupStartingGrid functions to log when they run, to compare a fresh-save launch with a normal one.
grid2 results (24 Nov 00:05, 00:19): the first launch after starting the game took the relayout path twice without
deleting the save (slot #5 kept); both were broken (all cars on one spot, red flag), and quitting to the menu froze in
StateUnloadSession (stall report: the loader at state 5, Sess_Module_OnSessionUnloaded) until the game was ended.
Game_GridSetup_SetupStartingGrid did run in the broken launch (inside BuildSessionSettings, this = a component next to the
save slot one); the EventList / FeaturedEvent setups did not. Both 2011 launches raced 18 of 24 entrants. The game DB
says swofty_f1's race group grid_type is "qualifying" and grid_type_no_qualifying "weightedRandom" (a normal launch shows
a shuffled grid). So the 1.0.6 workaround ("quit and launch again") is wrong when the quit freezes. wk1 dumps the
weekend (0x2a0 bytes) and its first session entry (0xa0) before and after the season build, after the entrants and
after a relayout, to diff a kept weekend with a laid-out one.
wk1 results (24 Nov 00:37-00:53): after Brad started one normal Grand Prix race from the game's menu (to the grid,
then quit), every Instant Race had 1 session in the weekend BEFORE the season build, kept it (no relayout) and was
good, including the first launch after restarting the game (00:52). So: the first-race bug happens when the Grand Prix
save's weekend has never been set up by the game's own Grand Prix start; the game saves that weekend with the save, and
Instant Race keeps it from then on. Workaround (in the README for players): start one normal Grand Prix, get to the
grid, quit. Fix for 1.0.7: do what the game's Grand Prix start does to the weekend; next test logs the game's own
Grand Prix start (wraps on Game_CustomSeason_RebuildFromRounds and Sess_WeekendSetup_LayoutWeekends with weekend dumps).
gp1 results (24 Nov 01:14-01:19, fresh save): the game's own Grand Prix start never calls
Game_CustomSeason_RebuildFromRounds or Sess_Build_DevWeekendStructure; it lays the weekend out with
Sess_WeekendSetup_LayoutWeekends (same component, vtable game+0x567fc60) many times (clearing it to 0 sessions and laying
it out again, last at 01:15:32), then Game_GridSetup_SetupStartingGrid (same this/arg as ours). Instant Race on a fresh
save: no season or weekend exists before the build (current weekend null); RebuildFromRounds creates the 'custom_season'
and a weekend with 0 sessions; our LayoutWeekends gives 1 session. The race session entries are the same apart from the
entrant count (+0x28: 20 vs 22) and the race length byte (+0x90: 2 = 3 laps vs 4 = 25%). Yet the grid is in vehicle
order and the phantom lap comes at 45 s. So the weekend is not the difference: the season is. A season RebuildFromRounds
creates from nothing is never started (Game_Season_Start: season +0x538 0 NotStarted -> 1 InProgress, plus lists).
gp2 logs the season's +0x538 at our build and at session live, and wraps Game_Season_Start and
Game_CustomSeason_RefreshFromSetup to see what the game's Grand Prix start calls.
gp2 run 1 (24 Nov 01:28-01:38): Game_Season_Start and Game_CustomSeason_RefreshFromSetup are not exported bindings, so
only the +0x538 log works. Brad ran a normal Grand Prix, deleted the save, then F7 in the same game session: the game's
season and weekend were still in memory (weekend 1 session before our build), season +0x538 = 1 at our build and at
session live, clean grid (20 in order, none within 3 m), no freeze on exit. That is the good case.
NEXT: the bad case with gp2: restart the game with no Grand Prix save, F7 straight away, and read +0x538. If it is 0,
start the season (or have the game do it) after RebuildFromRounds creates it.

gp3 (24 Nov 21:52-22:13, launcher 1.10.43): game restarted, F7 straight away, no Grand Prix menu: the weekend already had
1 session before our build (the game never laid one out this session, so it came from the save), season +0x538 = 1, no
relayout, both launches good. The weekend the game's own Grand Prix start sets up lives in the profile save and
survives deleting the Grand Prix slot (except in the gp1 run, where it was null after the delete; not understood).
Brad's profile can no longer show the bug, so the fix cannot be tested here. Short race fix confirmed: Custom 5 laps
gave race length 3 (5 laps), the game's build had 5, fuel fine, no compulsory stop (Brad drove the 5 laps).
Correction: Racingfox's +0x2cf07f2 crashes are NOT this bug. His 1.0.6 crash (and the first 1.0.4 one) took no
relayout: the weekend had its session. Cause unknown; ask the Swofty bot about fn 0x142cee9c0.
rc1 (24 Nov 22:20-22:31): the guard stopped the launch after the season build. The Grand Prix menu then showed the
stopped race as a saved Grand Prix; Brad deleted it, started a normal Grand Prix (race session), and quitting froze in
StateUnloadSession (stall report: loader state 5, Sess_Module_OnSessionUnloaded, waiting for modules to answer
unload-ready), the same freeze as the broken first race. So never stop after the build. rc2 checks the weekend before
anything changes (no weekend or 0 sessions = never set up; gp1 had none, good launches had 1) and keeps the 1.0.6
relayout after the build (slot1/fuel4 had good relayout launches, so it is not always broken).
rc2 (24 Nov 22:36-23:06): the stop came before any change (no Grand Prix save from it). Continuing the Grand Prix
saved during the rc1 test froze on quitting again (that save carries rc1's half-built state); after deleting it, a
new normal Grand Prix quit in 2 s, then Instant Race (2010 Jerez, 5 laps, 24 racers in order, none stacked at 5/20 s)
quit in 3 s. Shipped as 1.0.7. Still open: a real test on a profile that has never started a Grand Prix.
 9 Surprise launches back to back on Brad's PC): every grid good, BuildSessionSettings once per
launch, never laid out again, no cars stacked. The grid problem does not reproduce on 1.0.5 here; grid logging stays
in so player reports show it.

## 4b. Track slots that don't load (suzuka24)
`suzuka24` is listed as a track but is a spare slot the Fuji Speedway add-on (dromodf.fuji-speedway) maps into; Swofty
logs "/slot game/suzuka24 does not resolve in track". Picking it loaded a half-built track at 1 fps (20 Nov 03:35, no
way back to the menu). Hide slots that don't resolve, or show them by the add-on's name when they do.

## 5. Starting grid slot
Option to choose where the player starts (P1 to the back), or random.
- Swofty docs: `session.grid.build` event (`Game_GridSetup_SetupStartingGrid`) and the grid placement functions in `std/systems/sessions`.

## 6. Other languages (planned for 1.0.7)

Today all of Instant Race's own text is English, written into the code: the F7 menu, buttons, toasts and the loading
card. Season, team, driver and track names are prettified from the game's ids, so they don't follow the game's
language either.

- Move every UI string into one table keyed by language, English as the fallback.
- Language choice: an option in the F7 menu, remembered in addon storage. Following the game's own language setting
  needs a way to read it; the Swofty SDK's `swofty::lang` only overrides game strings and has no "current language"
  call, so that would be extra RE work (later, optional).
- First languages: French, German, Spanish, Italian, Portuguese (BR), Polish. Check accented text in the overlay's
  built-in font on the PC. Japanese / Chinese only if that font has the glyphs.
- Track names could use the game's own localized names (lng_* keys) instead of prettified ids, if they can be read.
- Ask native speakers on the Discord to check the translations before release.

## Smaller items
- Start-up hitch: resolve_all_game_addrs runs ~20 pattern scans over the whole exe on the first on_frame (~1.1 s,
  seen in Swofty Send Logs). Move it to a background thread at load, or scan once into a cache.
- (Done in 1.0.3) Ultrawide / triple screens: the "[F7] Instant Race" prompt is drawn at the far right of the whole display, while the game draws its menus in a centred 16:9 area (seen on 7680x1440). Anchor the prompt (and check the menu panel and loading card) to that centred 16:9 area: when width/height > 16/9, use x0 = (width - height*16/9) / 2 and w = height*16/9.
- Team picks that don't exist in the chosen era's grid fall back to another car: limit the car list to the grid actually used (`sw_f1_<year>`). (HRT and Toro Rosso cases fixed in `next`.)
- Release the weekend-structure list entries after handing them to the service (Swofty's direct start does this); currently a small leak per launch.

## Launcher updates checked
- **24 Nov: up to 1.10.43** (Brad updated from 1.10.35 to 1.10.43 that evening). None of Instant Race's bindings changed
  or went away. Useful: 1.10.40/1.10.43 write a load-stall report naming the step the game waits on when a session gets
  stuck while unloading (after 60 s), which covers the first-race freeze on quit. 1.10.30 hands Melbourne's record to a
  track data lookup by an unknown name (`[tracks] track data lookup guard:`), a different lookup from our track data set
  crash. 1.10.39 fixed Continue on a Grand Prix save at Austria, Silverstone or Zandvoort Reverse with no track addon on
  (closed the game on 1.10.30 to 1.10.38). 1.10.34/1.10.36 add live data (race lap count, fastest lap holder, pit window,
  driver names) that could replace some of our grid diagnostics.

## Player reports to look at
- **Tyre strategy and fuel (reported 20 Nov):** a player could not change their tyre strategy and their car was under-fuelled. Likely the race-only weekend / custom lap count not reaching the game's fuel and strategy setup. Need their instant_race.log, session folder, and the era/track/race length.
- **EA version, likely fixed by 1.0.5:** the 1.0.4 byte pattern for g_Game_Manager probably found the wrong global on the
  EA exe (mgr=0x146185008, 0 loaded entrant slots); 1.0.5 takes g_Game_Manager and everything else from the launcher's
  EA binding set, which lists the same exports as Steam. Needs an EA player to try 1.0.5.
- **EA version (report from 21 Nov, launcher 1.10.21):** every 1.0.4 byte pattern matched on the EA exe, but the
  launch stopped at "loaded slots in EntrantsSystem: 0" / "No loaded entrant list found" (68 lists in the DB). An
  EntrantsSystem offset or lookup probably differs on EA; Brad has no EA copy to test.
- **One-off load crash (Racingfox2309, 22 Nov 08:46, 1.0.4):** first Instant Race of the session (Melbourne, f1_2025,
  wet, Surprise); null virtual call at F1_25.exe+0x2cf07f2 on the UI thread as flow #7c8bb060 entered (driver names
  screen), 11 s after Instant Race's last step. Did not repeat: a normal wet Grand Prix at Melbourne, a dry Instant
  Race and a wet Instant Race all loaded afterwards. Watch for the same address in future logs.
- **Crash attributed to Instant Race (report-05, 21 Nov):** access violation at F1_25.exe+0xaf76f1 inside the game's
  own Game_ChampionshipGameMode_BuildSessionSettings during a normal Grand Prix; Instant Race's wrap was only passing
  through (no launch that session), but crash reports name instant_race.dll because the wrap is on the stack.
- **Swofty bot on +0x2cf07f2 (24 Nov 22:23):** 0x142cee9c0 is the game's create-session function (vtable slot 8 of a
  session object, called from Sess_Loader_Update); it picks game/flow_race.xml etc. from the session settings and stores
  a mode object at this+0xf0. The crash is `mov rcx,[this+0xf0]` = null: the settings type at [[this+0x220]+0x1a8] hit
  none of its branches (inferred), or the allocation failed. 4 reports at this site, all with Instant Race on, 0
  without. Next: ask which settings field feeds [[this+0x220]+0x1a8] and its valid values, then log it in our
  BuildSessionSettings wrap on every launch (and correct it if it is out of range).
- **Bot follow-up (24 Nov 22:28):** [this+0x220] is the game-state object; [[state+0x1a8]] is the SessionSettings
  (u8 ruleset at +0, u32 session id at +4, the same fields BuildSessionSettings writes; same instance inferred). Create
  session: ids 15..17 -> flow_race.xml, then the mode object is made only for rulesets 1 and 4..15; 0, 2 and >= 0x11
  make none (0x10 flow_dev also none, 3 a dev object). Time Trial writes ruleset 2, so a stale 2 or 0 fits (inferred).
  All 4 reports are Racingfox's. `next` now logs the ruleset on every BuildSessionSettings and warns on a race session
  with a ruleset that gets no mode; rs1 tests it (normal Grand Prix race, Time Trial, then Instant Race). Once the
  stock Grand Prix value is known, correct a bad one in the wrap.
- **rs1 (24 Nov 23:14-23:26):** a normal Grand Prix race wrote ruleset 1; Time Trial then Instant Race also got 1 at
  our BuildSessionSettings (no crash). So the build itself writes a good value; Racingfox's bad value (if that is the
  cause) appears later or in another copy. 1.0.7: a bad ruleset is set to 1 at the build, and the settings are watched
  every frame until the session is live and set back to 1 if they change. Not proven to fix his crash; his next log
  says ("Race session ruleset changed to ...").
- **2011 grid: 18 racers and a freeze on quitting (24 Nov 23:42, 1.0.7 release build):** f1cg_f1_2011 at Catalunya,
  25 cars but 18 racers, clean grid, ruleset 1; quitting froze in StateUnloadSession (stall: loader state 5,
  Sess_Module_OnSessionUnloaded). Same as grid2's two 2011 launches (18 of 24 racers, freeze) and the normal Grand Prix
  Brad started in the rc1 test, which had a 2011 grid and froze on quitting twice without Instant Race in it. So the
  2011 season freezes on quitting in a normal Grand Prix too: not an Instant Race bug. Asked the Swofty bot.
- **Racingfox never plays Time Trial (25 Nov):** so a stale Time Trial ruleset (2) is ruled out. Left: a ruleset that
  is fine at our build and changes before the race is created (1.0.7 corrects it and logs "Race session ruleset changed
  to ..."), a different SessionSettings copy than the one our wrap sees, or the mode allocation failing. If he crashes
  on 1.0.7: ruleset 1 at the build and no "changed" line means the 1.0.7 check did not reach the cause; ask the bot.
- **Short races now ask for a pit stop (Racingfox2309, 24 Nov, 1.0.6):** "the game doesn't detect 5 lappers anymore, I
  have to make a pit stop". 1.0.6's fuel fix sends 25% as the weekend race length for anything up to 25%, and 25% brings
  the 25% race rules (compulsory stop with two dry compounds; game setup mode tyre_regulations.race0_length_requirement).
  1.0.5 and earlier always sent 5_laps, which has no compulsory stop. Fixed in `next` (not yet tested in game): a race
  under 25% takes the smallest of 3, 5, 10 or 15 laps that covers it (the lap values the game's race rule tables have
  rows for); the log names the value.
- **Same crash a third time (Racingfox2309, 24 Nov 08:01, 1.0.6):** f1_2025, Hungaroring, Surprise, 70 laps. Track data
  matched (season 7, 49 tracks), race length 7, weather and start time set; then the null virtual call at
  F1_25.exe+0x2cf07f2 (fn base+0x2cee9c0, `mov rax,[rcx]` with rcx 0) as flow #7c8bb060 (the Race flow state, per
  Swofty's session notes) entered, 14 s after StateLoadSession and before the session went live. Launch #1 of the game
  session again. All three cases: the first Instant Race of a game session, at the Race flow entry, and on 22 Nov it did
  not repeat after a normal Grand Prix. I first took this for the first-race bug, but none of these launches took the relayout path (see gp3),
  so the cause is still unknown. The bot's 23 Nov pair (1.0.5, reading 0x268) is the track data crash fixed in 1.0.6.
- **Crash while loading (Racingfox2309, 1.0.3):** 2026 season, Silverstone reverse layout, Toro Rosso. Null call at F1_25.exe+0x2cf07f2, 16 s into loading, no Instant Race frame on the stack, not repeated (their 2024 Zandvoort and 2019 Hockenheim races loaded and finished). Check whether the reverse layout or the 2026 grid is involved.
- **Aston Martin seated at "Force India" (same player): not a bug.** Swofty's 2021+ grids keep Aston Martin in the
  Force India slot (sw24_force_india is the 2024 Aston Martin: Stroll/Alonso; the 2024 grid has no other candidate).
  The one real wrong-car case (2022 Aston Martin -> Mercedes on 19 Nov 00:44) was a dev build from before the slot aliases.
- **Fixed in `next`:** HRT (2012) put you in the Caterham (3-letter team names were never matched; now an exact
  slot-name match on the team's first word). 2013/2016 Toro Rosso could put you in the Ferrari (the game's ids spell it
  "torro_rosso"; alias added; confirmed in game 20 Nov: 2013 Toro Rosso seated in sw13_toro_rosso). Checked with a simulation of the matcher over 29 real team ids: no other pick changes.
- **Unverified:** the 2024 RB team's f1cg id never appears in the logs; if it is "rb-...", it would fall back to another
  car. Needs one log line with that pick.

## Swofty 1.10.0 notes (20 Nov)
- Career on the main menu now opens Swofty's own list of careers (`SWOFTY_RESUME_LIST=0` restores the game's menu). It
  hooks Game_F1WorldActivityContainer_HandleCommand, which Instant Race also wraps; check both still work together.
- New std libraries worth a look: weather setup (`session.weather.setup`, scripted forecasts per dataset; could replace
  our own weather hook and give real changing weather), tyre damage and pit lane (for the tyre strategy / fuel report),
  race control and standings.
- None of Instant Race's bindings are on the deprecated list.
- 1.10.1-1.10.11: nothing affecting Instant Race. 1.10.2 removed the three deprecated bindings (not used by us).
  1.10.8 adds `live::car_detail` (tyre wear, tyre age, stint laps, lap and sector times, grid position) on Steam: useful
  for the tyre strategy / fuel report.

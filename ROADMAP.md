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

## 1. Time of day
Let players pick the race start time (or random).
- Same hook as the weather fix: `Sess_GameWeatherSystem_SetupSession` reads `WeatherSessionSettings`.
- Start-time fields (Swofty RE, `re/db/structs/WeatherSessionSettings.h`): `tod_flag_a` +0xb10 / `tod_flag_b` +0xb11 (both 0 = use the per-dataset start presets), per-dataset block at +0xb28 + d*8 (u8 start preset 0..7), `tod_start_preset` +0xb19.
- Some tracks may only allow fixed times (night races); check what the game does with each preset.

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
- Next (diag4, native/src/instant_race_diag4.cpp on the dev PC, read-only): 1.0.3 behaviour plus memory snapshots of
  StateFrontendWorld, StateFrontend, e4ef00ba and 40b45c37 (0xd40 bytes each, written to
  %LOCALAPPDATA%\Swofty\instant_race_snap_*.bin) on the menu at start-up, at the launch, on the menu after the race,
  at the career start, and whenever StateFrontendWorld reports a new link. Diffing start-up vs after-race should show
  the field that keeps the exit decision. Then decide between resetting that field and clearing the interrupt the
  instant StateFrontend's onStateExit finishes (Swofty's FTUE approach).
- The menu fade bit 0x80000000 that Instant Race clears by hand never appears in a normal Grand Prix or career start;
  it probably comes from the same interrupt exit. 
- Swofty's own direct start (launcher 1.9.91, native/src/core/direct_start.cpp) raises the same interrupt only on
  StateFTUE as it leaves, and clears it the moment StateFTUE's onStateExit finishes, so its parents never see it.
  Instant Race raised it on the first state its hook saw (e4ef00ba, a parent) and kept it set for ~3 s until
  StateFrontendWorld had no child, long enough for the parents to latch it.

## 5. Starting grid slot
Option to choose where the player starts (P1 to the back), or random.
- Swofty docs: `session.grid.build` event (`Game_GridSetup_SetupStartingGrid`) and the grid placement functions in `std/systems/sessions`.

## Smaller items
- (Done in 1.0.3) Ultrawide / triple screens: the "[F7] Instant Race" prompt is drawn at the far right of the whole display, while the game draws its menus in a centred 16:9 area (seen on 7680x1440). Anchor the prompt (and check the menu panel and loading card) to that centred 16:9 area: when width/height > 16/9, use x0 = (width - height*16/9) / 2 and w = height*16/9.
- Team picks that don't exist in the chosen era's grid fall back to another car: limit the car list to the grid actually used (`sw_f1_<year>`). (HRT and Toro Rosso cases fixed in `next`.)
- Release the weekend-structure list entries after handing them to the service (Swofty's direct start does this); currently a small leak per launch.

## Player reports to look at
- **Tyre strategy and fuel (reported 20 Nov):** a player could not change their tyre strategy and their car was under-fuelled. Likely the race-only weekend / custom lap count not reaching the game's fuel and strategy setup. Need their instant_race.log, session folder, and the era/track/race length.
- **Crash while loading (Racingfox2309, 1.0.3):** 2026 season, Silverstone reverse layout, Toro Rosso. Null call at F1_25.exe+0x2cf07f2, 16 s into loading, no Instant Race frame on the stack, not repeated (their 2024 Zandvoort and 2019 Hockenheim races loaded and finished). Check whether the reverse layout or the 2026 grid is involved.
- **Aston Martin seated at "Force India" (same player): not a bug.** Swofty's 2021+ grids keep Aston Martin in the
  Force India slot (sw24_force_india is the 2024 Aston Martin: Stroll/Alonso; the 2024 grid has no other candidate).
  The one real wrong-car case (2022 Aston Martin -> Mercedes on 19 Nov 00:44) was a dev build from before the slot aliases.
- **Fixed in `next`:** HRT (2012) put you in the Caterham (3-letter team names were never matched; now an exact
  slot-name match on the team's first word). 2013/2016 Toro Rosso could put you in the Ferrari (the game's ids spell it
  "torro_rosso"; alias added; confirmed in game 20 Nov: 2013 Toro Rosso seated in sw13_toro_rosso). Checked with a simulation of the matcher over 29 real team ids: no other pick changes.
- **Unverified:** the 2024 RB team's f1cg id never appears in the logs; if it is "rb-...", it would fall back to another
  car. Needs one log line with that pick.

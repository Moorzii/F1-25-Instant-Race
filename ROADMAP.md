# Instant Race roadmap

Planned work, roughly in order. Notes under each item are where to start.

## 0. Ghost grid on the first load (intermittent)
On a profile that never played a Grand Prix, cars sometimes spawn inside each other when the race first loads;
restarting the session always fixes it. 1.0.3 fixed the missing race session (the weekend is laid out again with
Sess_WeekendSetup_LayoutWeekends), but not this.
- Game_GridSetup_SetupStartingGrid / DriverBasedGrid run once at the load (their 32-byte output stays 0xff on good
  and bad loads alike) and not at all on a restart, so the placement comes from somewhere else.
- Next: trace the per-car placement (Game_GridSlot_PlaceVehicle, Game_VehiclePlacementSystem) on a ghosted first
  load and on the restart, and compare.

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

## 4. Second Instant Race (or any other race) without restarting the game
Currently blocked with a "restart the game" message. A player also reports that starting a normal race from the
game's own menus after an Instant Race black-screens on loading; likely the same root cause (the menu/session
hand-off after our race), so treat them as one bug. Ask for that player's Send Logs bundle to confirm.
- Check whether the mod's stage returns to idle after the race (no "Returned to main menu" line has been seen in
  logs) and whether anything left on the Grand Prix entity (cleared rounds, entrant list, setup mode, the session
  plan handed to the weekend service) breaks the next Grand Prix built from the menu.
- On a second launch StateFrontend (957880c6) goes to status 3 (exiting) and never finishes; its exit never starts the menu fade-out, while the first launch does it within ~10 ms. Forcing the fade did not help.
- Next step: read the game's StateFrontend exit code (decrypted at runtime, so the mod would need to dump it) to see what it waits for.

## 5. Starting grid slot
Option to choose where the player starts (P1 to the back), or random.
- Swofty docs: `session.grid.build` event (`Game_GridSetup_SetupStartingGrid`) and the grid placement functions in `std/systems/sessions`.

## Smaller items
- (Done in 1.0.3) Ultrawide / triple screens: the "[F7] Instant Race" prompt is drawn at the far right of the whole display, while the game draws its menus in a centred 16:9 area (seen on 7680x1440). Anchor the prompt (and check the menu panel and loading card) to that centred 16:9 area: when width/height > 16/9, use x0 = (width - height*16/9) / 2 and w = height*16/9.
- Team picks that don't exist in the chosen era's grid fall back to another car: limit the car list to the grid actually used (`sw_f1_<year>`).
- Release the weekend-structure list entries after handing them to the service (Swofty's direct start does this); currently a small leak per launch.

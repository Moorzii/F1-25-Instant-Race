# Instant Race

Skip the menus and jump straight into a race in F1 25 (Swofty Launcher addon).

## How to use
1. Press **F7** on the main menu (you can change the key in the addon's settings).
2. Pick an era, car and driver, track, weather and race length, or press **Roll All Random**.
3. Press **START INSTANT RACE**.

**Surprise mode:** turn on **Surprise** and the era, car and driver, circuit and weather are picked at random and kept hidden until the race loads. Race length still uses your setting.

Any Swofty season you have installed (2012 onwards, plus F1 25) appears in the era list automatically, and installed track addons appear in the circuit list. The Swofty season packs provide the period grids, cars and liveries.

## Known limitations (beta)
- **AI difficulty comes from your normal Grand Prix setting** (Grand Prix > Settings > Difficulty). The mod doesn't change it yet.
- **Restart the game between Instant Races.** Starting a second Instant Race in the same game session is blocked, because it currently hangs on the menu. The mod shows a reminder when you press Start.
- If the car you pick isn't on the selected era's grid, you'll be placed in another car on that grid.
- Going back to the main menu after a race can take 1-3 minutes; that is the game unloading the race.
- Game updates can stop the mod working until it's updated.

## Changelog
**1.0.2**
- Fixed the F7 key and the "[F7] Instant Race" button never appearing on setups with many addons (log: "flow hook: no stub memory near ..."), and the stutter while it retried.

**1.0.1**
- Fixed cars spawning inside each other on the grid ("ghost grid") and the lap counter jumping to lap 2 with fastest-lap spam on the first crossing at some tracks.
- Fixed the Wet/Dry setting: it now actually sets the race weather.
- Fixed "Core game signatures not found" when Career Mode Editor (or another addon hooking the game-mode switch) is enabled.
- Fixed teams like Williams-Mercedes, Aston Martin, Renault/Alpine and Marussia putting you in the wrong car.
- Added the addon icon.

**1.0.0**
- First release (beta).

## Reporting a bug
Please include the log file:
`%LOCALAPPDATA%\Swofty\instant_race.log`
(paste that into the Windows Explorer address bar to open the folder). Say which era, track and car you picked, and what happened.

## Building from source
The addon is a single C++ file, `native/src/instant_race.cpp`, built against the Swofty addon SDK headers (from the Swofty Launcher, placed in `native/include`):

```
zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Iinclude -shared -s -o instant_race.dll src/instant_race.cpp
```
(run from the `native` folder). Put the DLL in `native/` next to `swofty.json` to package it.

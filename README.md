# Instant Race

Skip the menus and jump straight into a race in F1 25 (Swofty Launcher addon).

F1 2017 had an Instant Race option that dropped you into a race with a random driver at a random circuit. Players have
asked for it back ever since; this addon brings it to F1 25 and lets you choose (or roll) everything about the race.

## How to use
1. Press **F7** on the main menu (you can change the key in the addon's settings).
2. Pick an era, car and driver, track, weather, time of day and race length, or press **Roll All Random**.
   Race length is a lap slider (1-100) or **Short**, **Half** or **Full Distance**, which follow the real race length of
   the circuit (Monaco 78 laps, Spa 44, ...).
3. Press **START INSTANT RACE**.

**Surprise mode:** turn on **Surprise** and the era, car and driver, circuit, weather and time of day are picked at random and kept hidden until the race loads. Race length still uses your setting.

Any season you have installed appears in the era list automatically: Swofty's seasons, F1 25, and season packs from other authors. Installed track addons appear in the circuit list.

Needs Swofty Launcher 1.10.29 or newer: Instant Race uses only the launcher's own bindings.

## Known limitations
- **AI difficulty comes from your normal Grand Prix setting** (Grand Prix > Settings > Difficulty). The mod doesn't change it yet.
- **Instant Race needs one normal Grand Prix on your save first.** On a profile where the game has never set up a
  Grand Prix race itself (for example a new profile), Instant Race can't build a working race, so it stops and tells
  you: start one normal Grand Prix from the game's Grand Prix menu, get to the grid, then quit. Instant Race works from
  then on, also after restarting the game.
- If the car you pick isn't on the selected era's grid, you'll be placed in another car on that grid.
- Going back to the main menu after a race can take 1-3 minutes; that is the game unloading the race.
- Add-on tracks can take several minutes to load (Laguna Seca about 7); the loading card says so. Leave it running.
- Weather is fixed for the whole race; changing weather (dry to wet) isn't supported yet.
- Game updates can stop the mod working until it's updated.

## Changelog
**1.0.7**
- **Short races:** races shorter than a quarter of the real distance (a 5-lap race, say) no longer ask for a compulsory
  pit stop. Since 1.0.6 the game treated every race up to 25% as a 25% race, with its two-compound pit stop rule. Fuel
  still covers the full race.
- **No more broken first race:** on a profile that has never started a normal Grand Prix, Instant Race used to start
  a broken race (empty Starting Grid screen, cars inside each other, a lap counted at the start, a freeze when
  quitting). It now checks first and, before changing anything, tells you to start one normal Grand Prix first.
- **Crash as the race starts:** guards against a crash a player hit on their first Instant Race after opening the game,
  about 15 seconds into loading. Instant Race now checks the race's rule set while it loads and keeps it valid.

**1.0.6**
- **Fuel fixed:** every car (yours and the AI's) was fuelled for a 5-lap race whatever distance you picked, so longer
  races ran short of fuel after a few laps. The game now fuels the race for the distance you chose.
- **Loading crash fixed:** some extra seasons made the game crash on the loading screen because the game had no track
  data for them in a Grand Prix. Instant Race now checks first and runs them with track data that exists.
- **Add-on track note:** the loading card says when an add-on track can take a few minutes to load.
- More detail in the log for bug reports (grid positions at the start of each race).

**1.0.5**
- **Time of day:** Track Default, Sunrise, Morning, Midday, Afternoon, Sunset or Night (night falls back to sunset at
  tracks without floodlights).
- **Six weather options:** Clear, Light Cloud, Overcast, Light Rain, Heavy Rain and Storm, instead of just Dry and Wet.
- **Race distance:** the lap slider goes up to 100, plus Short, Half and Full Distance buttons that use each circuit's
  real race length.
- **More seasons:** season packs from other authors now show up in the era list too.
- **Built on Swofty Launcher's bindings:** no own code patches or game searches any more, so it meets the Addon Browser
  rules, starts without the short freeze on game start, and should keep working across launcher updates. Needs
  launcher 1.10.29 or newer.

**1.0.4** (first full release)
- You no longer need to restart the game after an Instant Race. Career, Grand Prix and another Instant Race now all start normally afterwards, instead of a black screen or a hang on the main menu.
- Fixed HRT (2012) putting you in the Caterham, and Toro Rosso (2013 and 2016) sometimes putting you in the Ferrari.
- The broken "suzuka24" circuit is no longer offered. It's a spare track slot that loaded at about 1 fps.

**1.0.3**
- Fixed the race having no session on profiles that have never played a Grand Prix, which caused the lap counter to jump to 2/2 and fastest-lap spam on the first crossing of the line.
- The "[F7] Instant Race" button now sits in the game's menu area on ultrawide and triple-screen setups, instead of at the far right of the display.
- Removed an unused weather hook.

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

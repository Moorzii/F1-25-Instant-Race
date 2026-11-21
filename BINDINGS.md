# Instant Race: launcher bindings audit

Audit of every place Instant Race touches game code, against the Addon Browser rule "addons containing code must use
bindings provided by the launcher". Addresses are RVAs on Steam F1 25 1.0.145 (build 26566308), checked against
Swofty Launcher 1.10.13's bindings (the ones the launcher bound on that build, and the `re/bindings` catalogue).

## Already through launcher bindings

| Use | Binding |
|---|---|
| Wrap: session settings for the Grand Prix weekend | `Game_ChampionshipGameMode_BuildSessionSettings` |
| Wrap: weather and start time | `Sess_GameWeatherSystem_SetupSession` |
| Wrap: main menu commands | `FeHome_Handle_Command` |
| Call: re-lay out a weekend's sessions | `Sess_WeekendSetup_LayoutWeekends` |
| Call: switch game mode | `Game_Game_SetCurrentGameMode` |
| Call: options lookup (AI difficulty) | `Game_OptionsStore_FindOption`, `g_Game_OptionsStore` |
| Call: game string from C string | `Ne_Make_StrFromCStr` |
| Data | `g_Game_Manager`, `g_Game_Root`, `g_Game_MessageBus` |
| Menu overlay | `swofty::overlay` (launcher overlay API) |

From 1.0.5 these are tried first; Instant Race only falls back to its own byte patterns when the launcher does not
bind a name on the running build.

## Needed from the launcher

### Mid-function hook sites (these replace Instant Race's two custom code patches)

| What | RVA | How Instant Race uses it |
|---|---|---|
| Flow state machine: after a state's update returns its link (`call [rax+0x20]`, link at `[rsp+0x40]`, then `cmp eax, [stay]`) | `0x4427248` | Reads and sets the link the state returned. This is how the launch leaves the menu (exit link on the menu leaf states, session link on StateFrontendWorld), completes the load sync and returns to the menu. |
| `F1WorldActivityContainer::HandleCommand` +0x1e (`mov rdi, r8`) | `0x1a7955e` | Captures the activity container view model (r8). |

Alternative to the first site: function bindings for the state updates Instant Race steers, which the catalogue
already has but the launcher does not bind on this build: `Game_StateFrontend_OnStateUpdate`,
`Game_Flow_TempLOGCallingStateFrontendWorld` (StateFrontendWorld::onStateUpdate), plus the menu leaf state and
StateLoadSession updates.

### Functions

| What | RVA |
|---|---|
| `F1WorldActivityContainer::HandleCommand` (not in the catalogue; a wrap would replace the site above) | `0x1a79540` |
| Vm fire route (UI route command; `Vm_Fire_Route` is not bound on this build) | `0x42c33f0` |
| Find game mode by predicate | `0x7d22c0` |
| Set game setup mode | `0x79f340` |
| Entrant list to stat id | `0x242a2e0` |
| Championship rounds: clear | `0x420f330` |
| Championship rounds: add track | `0xb99430` |
| Build season (championship component, 0x298 bytes) | `0x12078d0` |
| Fill entrants from an entrant list | `0x2461e30` |
| Weekend structure / session plan | `0xb02640` |
| Message bus: post begin | `0x24e4790` |
| Message bus: post end | `0x24e4b00` |
| `Game_FrontendSceneManager_DespawnMainScene` (in the catalogue, pattern matches, not bound) | `0x28dbe50` |
| `Vm_NodeSet_Bool` (in the catalogue, pattern matches, not bound) | `0x973e70` |

### Data

| What | RVA |
|---|---|
| Grand Prix game mode predicate | `0x77d030` |
| Weekend service component description | `0x605cda0` |
| Rounds component description | `0x605cd40` |
| Message vtables (begin / end) | `0x578f618`, `0x578f640` |

Struct field offsets Instant Race reads and writes (flow state status, weekend sessions, weather session settings,
menu exit latches) are data layout, not hooks, and are listed in ROADMAP.md.

# Instant Race: launcher bindings

Since 1.0.5 Instant Race reaches the game only through Swofty Launcher bindings (Addon Browser rule: "addons
containing code must use bindings provided by the launcher"). It has no byte patterns, fixed addresses or code
patches of its own. Needs Swofty Launcher 1.10.29 or newer; on an older launcher it refuses to launch and says to
update.

The bindings Instant Race needed were added to the launcher in SwoftyLauncher PRs #1467 and #1514 (Moorzii) and
#1515 (verify pass and renames).

## Hooks (mixins)

| Kind | Binding | What Instant Race does there |
|---|---|---|
| Site | `site_FeFlow_AfterStateLink` | Reads and replaces the link a flow state returns (`[rsp+0x40]`, state in `rdi`): leaves the menu, completes the load sync, notices the return to the menu. |
| Wrap | `Game_ChampionshipGameMode_BuildSessionSettings` | Race length and session settings for the weekend. |
| Wrap | `Sess_GameWeatherSystem_SetupSession` | Weather and start time. |

## Calls and data

| Use | Binding |
|---|---|
| Game objects | `g_Game_Manager`, `g_Game_Root`, `g_Game_MessageBus`, `g_Game_OptionsStore` |
| Switch to the Grand Prix mode | `Game_Game_SetCurrentGameMode`, `Game_GameModes_FindByPredicate`, `Game_GameMode_IsGrandPrix` |
| Build the weekend | `Game_GameModeEntity_SetSetupMode`, `Vm_Node_Clear`, `Game_ChampionshipRounds_AddTrack_Thunk`, `Game_CustomSeason_RebuildFromRounds`, `Sess_Build_DevWeekendStructure`, `Sess_WeekendSetup_LayoutWeekends`, `k_Sess_WeekendSetupComponentDesc`, `k_Game_CustomTrackListComponentDesc` |
| Fill the grid | `Ent_Fill_EntrantsFromList`, `Game_MessageBus_PostEntrantChangeBegin`, `Game_MessageBus_PostEntrantChangeEnd`, `vt_Game_EntrantsDocumentOpenedMsg`, `vt_Game_EntrantsDocumentClosedMsg` |
| Load and hand over | `Vm_NodeSet_Bool`, `Game_FrontendSceneManager_DespawnMainScene` |
| Options | `Game_OptionsStore_FindOption` |
| Menu overlay | `swofty::overlay` (launcher overlay API) |

Struct field offsets Instant Race reads and writes (flow state status, weekend sessions, weather session settings,
menu exit latches) are data layout, not hooks, and are listed in ROADMAP.md.

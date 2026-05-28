# STATUS control-center phase gamma

Status: PASS
Area: Bottle and prefix manager foundation

## Implemented

- Added ControlCenterBottlePlanner and reusable bottle templates for games, 1C, AutoCAD, and office apps.
- Bottle paths are derived from /Users/timurtoby/Documents/MacRunner/Main/MacRunner/bottles through AppSettings defaults.
- Wine actions are represented as Process invocations, not linked engine calls.

## Remaining Limitations

- Planner creates safe invocations; it does not execute wineboot automatically in tests.

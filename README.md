# Project Accretion

Project Accretion is a Windows-first C++ hero shooter foundation focused on competitive feel: authoritative simulation, uncoupled rendering, high tick-rate correctness, and reproducible iteration for future collaborators.

## Current Milestone
- One playable Windows executable: `dist/Accretion.exe`
- One local player client connected to a local authoritative server over loopback UDP
- One damage hero with a no-ADS hitscan rifle
- One gray-box training range with static and moving dummies

## Build
1. Run `powershell -ExecutionPolicy Bypass -File .\scripts\bootstrap_toolchain.ps1`
2. Run `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`
3. Launch `dist\Accretion.exe`

## Controls
- `WASD`: move
- `Space`: jump
- `Left Ctrl`: crouch
- `Left Mouse`: fire
- `R`: reload
- `Esc`: toggle settings menu
- `F1`: toggle debug HUD
- `F2`: toggle collision and hitbox overlays
- `F3`: toggle artificial network delay
- `Alt+Enter`: toggle fullscreen

## Feature Log
1. Added a repo-local Windows toolchain bootstrap flow for CMake, Ninja, and MinGW-w64 so contributors can build without machine-wide setup.
2. Added shared collaboration docs: `AGENTS.md`, `README.md`, and `docs/architecture.md`.
3. Added a CMake-based Windows build pipeline that outputs a clickable executable at `dist/Accretion.exe`.
4. Added a single executable that hosts a local authoritative server and client over loopback UDP.
5. Added a fixed 256 Hz server simulation loop decoupled from rendering.
6. Added client-side prediction, snapshot reconciliation, and prediction error telemetry.
7. Added Overwatch-like baseline movement with jumping, crouching, raw mouse look, and no movement-based aim penalties.
8. Added a first-person hitscan rifle with a 30-round magazine, 10 shots per second, 20 body damage, 40 headshot damage, 1.5 second reload, and instant respawn.
9. Added Halley Range, a compact stylized training space with cover, elevation, distance lanes, and static plus moving dummies at 10 m, 20 m, 35 m, and 50 m.
10. Added a debug HUD with frame pacing, rates, correction stats, movement data, hit feedback, and optional network stress toggle.
11. Added a settings overlay with a dedicated mouse tab, video settings, and gameplay tuning fields.
12. Added telemetry logging for frametimes, tick jitter, and key gameplay events.

## Next Likely Steps
- Add stronger lag-compensation diagnostics and configurable packet simulation.
- Add richer dummy behaviors and aim-trainer scenarios.
- Add projectile weapon support and hero ability scaffolding.
- Add audio, key rebinding, and a more robust menu flow.


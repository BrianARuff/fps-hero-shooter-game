# Project Accretion Architecture

## Design Intent
Project Accretion starts as a local-only playable prototype, but it is intentionally shaped like an online shooter:
- The client and server communicate through loopback UDP, not direct function calls.
- The server is authoritative for movement, combat, dummy state, damage, and respawn.
- The client predicts its own movement and reconciles from authoritative snapshots.
- Rendering runs independently from gameplay simulation.

## Timing Model
- Server simulation target: `256 Hz`
- Server scheduling: absolute-time fixed-step cadence to reduce drift and make tick lateness measurable
- Client prediction target: fixed-step simulation matched to server cadence
- Rendering target: uncapped by default, optionally frame-capped through settings
- Timekeeping: monotonic high-resolution clock

Rendering must never alter movement, weapon cadence, hit registration, or collision behavior.

## Major Runtime Pieces
- `Win32 platform layer`
  - Window lifecycle
  - Raw input
  - Borderless fullscreen startup plus fullscreen and resolution changes
  - Telemetry file creation
- `D3D11 renderer`
  - Simple colored geometry for world, dummies, and debug overlays
  - Immediate-mode style 2D HUD and settings overlay
  - Row-major matrix upload path that matches the CPU-side transform math used by the fixed-function debug renderer
- `Local server`
  - Fixed tick loop
  - World simulation
  - Dummy movement
  - Weapon firing validation
  - Lag-compensated hitscan against dummy history
- `Client runtime`
  - Input sampling
  - Prediction
  - Snapshot ingestion
  - Reconciliation
  - First-person camera
- `Shared simulation`
  - Movement rules
  - Collision
  - Weapon timings
  - Health and respawn rules

## Networking Model
- Transport: loopback UDP on localhost
- Current topology: one client and one server inside one executable
- Packets:
  - client hello
  - input command
  - fire event
  - snapshot
  - hit confirm
- Snapshot requirement: authoritative snapshots must carry enough movement and weapon state for correct reconciliation, including grounded state and active weapon timers.
- Future-ready constraint: keep packet types and simulation rules valid if the server later runs in a separate process or on another machine.

## Simulation Rules
- Player baseline: `200 HP`
- Weapon baseline:
  - `30` rounds
  - `10` shots per second
  - `20` body damage
  - `40` headshot damage
  - `1.5 s` reload
  - no ADS
  - no damage falloff yet
- Movement baseline:
  - Overwatch-like responsiveness
  - jump + crouch in milestone one
  - no movement-based spread
  - no airborne accuracy penalty
  - no camera sway

## Debug and Telemetry
Always keep enough instrumentation to answer these questions quickly:
- Is render pacing stable?
- Is the server hitting 256 Hz?
- How much prediction error is being corrected?
- Are dummy hitboxes and lag compensation behaving correctly?
- What happened in the last few damage events?

Runtime telemetry is written to `telemetry/` and should remain lightweight enough for regular local playtests.
For render verification, the executable can also dump a startup back-buffer image to `telemetry/startup_frame.bmp` when launched with `--capture-startup-frame`.

## Documentation Rules
- Append new shipped features to the ordered list in `README.md`.
- Update this file whenever changing build assumptions, tick rates, packet flow, simulation ownership, or render/sim boundaries.
- Update `AGENTS.md` when a workflow becomes recurring.

## Startup Defaults
- Settings version `3` defaults to fullscreen-first launch so the initial playtest opens at the active monitor size instead of a smaller bootstrap window.
- Legacy configs are migrated forward on load so earlier local settings files do not keep forcing the old startup path.
- Gameplay cursor capture is armed on the first click-and-release into the range so startup and menu transitions do not inject an accidental camera turn.

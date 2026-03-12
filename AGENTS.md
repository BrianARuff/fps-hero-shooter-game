# Project Accretion Agent Guide

## Mission
- Protect the competitive shooter foundation first: fixed-step simulation accuracy, smooth frame pacing, authoritative server correctness, and low-latency input feel.
- Keep rendering and simulation decoupled at all times. Rendering convenience must not change gameplay outcomes.
- Treat this repository as multi-agent friendly. Any change should leave enough context for another contributor or AI agent to continue cleanly.

## Recurring Steps
1. Read `README.md` and `docs/architecture.md` before changing code that touches simulation, networking, rendering, timing, or settings.
2. Preserve the 256 Hz server simulation target unless a change is explicitly documented in `docs/architecture.md`.
3. Keep the project producing a clickable Windows executable at `dist/Accretion.exe`.
4. When you add a user-facing or developer-facing feature, append it to the ordered feature log in `README.md`.
5. When you change architecture, timing, packet flow, build workflow, or toolchain assumptions, update `docs/architecture.md` in the same change.
6. Build and smoke test before handing off:
   - `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`
   - Launch `dist/Accretion.exe`
7. Do not couple physics to render delta time. Movement, combat, collision, and weapon cadence must stay on fixed simulation steps.
8. Prefer additive docs over tribal knowledge. If you discover a recurring workflow, add it here.

## Current Conventions
- Project codename: `Project Accretion`
- First playable hero codename: `Mercury`
- First playable space: `Halley Range`
- First milestone scope:
  - Local authoritative server architecture inside one executable
  - Loopback UDP packet flow
  - Client prediction and server reconciliation
  - 256 Hz server simulation
  - Overwatch-like baseline movement with no movement-based aim penalties
  - Hitscan rifle, headshots, instant respawn
  - Training range with static and moving dummies
  - Debug HUD, settings menu, and telemetry logging

## Guardrails
- Do not add ADS, projectile weapons, hero abilities, or audio until they are intentionally scoped.
- Do not replace the local-authoritative approach with single-player shortcuts.
- Do not remove telemetry, debug visibility, or prediction/correction instrumentation without replacing them with something equivalent or better.


# Upstream foundation and integration record

## Source snapshot

- Project: [rawalakshay/fps-monitor-panel](https://github.com/rawalakshay/fps-monitor-panel)
- Imported branch: `main`
- Imported commit: `26ef701289704d347aa8ad6824d858dd19a6efdc`
- Local snapshot: `upstream/fps-monitor-panel/`
- License: GNU GPL-3.0-or-later

The imported FPS Monitor Panel is the **primary implementation and build foundation** of this repository. Its original copyright headers, license, build system, tests, dependencies and third-party notices are preserved.

## Architecture decision

The integrated payload is built from `upstream/fps-monitor-panel/source/`. That upstream code owns the ShellUI integration, overlay lifecycle, scene renderer, layout, FPS/frame-time pipeline, settings and payload startup.

The original project files in the root `src/` and `include/` directories are retained only as a source of reference for missing telemetry. The previous project's loader, injector, daemon and HUD renderer are not carried into the new payload architecture.

The root `CMakeLists.txt` delegates to the upstream source tree. The upstream build script remains available at `upstream/fps-monitor-panel/scripts/build.sh`.

## Telemetry boundary

Upstream telemetry retained without replacement:
- FPS, frame-time history, averages, lows, target tracking and hitch detection.
- CPU temperature and true per-core CPU sampling using idle-thread deltas.
- SoC sensor 0 temperature, displayed as the GPU/SoC temperature.
- GPU core rail power, displayed separately from temperature.
- System RAM page-table usage.

Telemetry added from the previous project:
- VRAM page-table usage using `get_page_table_stats(VM_SYSTEM, PAGE_TABLE_VRAM, ...)`, with a separate validity flag.
- Fan duty using the optional `sceKernelGetCurrentFanDuty` symbol, converted from its 0–1024 duty range into a percentage.

The previous project's aggregate CPU approximation is deliberately not used. Unavailable or invalid readings must remain unavailable rather than appear as zero.

## Known limits and validation

- Page-table type 2 and the units returned for VRAM require confirmation on hardware.
- Fan duty is unavailable if the optional firmware symbol cannot be resolved.
- SoC sensor 0 is not claimed to be a separately verified GPU-die temperature.
- GPU core rail power is not total SoC/APU power.
- No GPU busy/utilization counter is claimed.

GitHub Actions has successfully run the upstream host tests and compiled the PS5 ELF using the PS5 Payload SDK. This verifies the build, not the accuracy of telemetry on a physical console. On-console validation against PS5 firmware 13.60 remains outstanding.

# PS5 FPS Monitor Overlay

This project is based on **[rawalakshay/fps-monitor-panel](https://github.com/rawalakshay/fps-monitor-panel)**. The upstream project is the actual overlay foundation, not merely a reference implementation.

The imported upstream snapshot is pinned in `upstream/FPS_MONITOR_PANEL_UPSTREAM_COMMIT`. Its original source, build system, settings application, third-party notices and license are retained under `upstream/fps-monitor-panel/`.

## Architecture

### Primary foundation: FPS Monitor Panel

The shipped payload is built from the upstream implementation. It owns:

- ShellUI integration and overlay lifecycle.
- Scene/layout renderer and all overlay presentation.
- FPS sampling and publication.
- Frame-time timeline, AVG/LOW statistics, hitch detection and target tracking.
- CPU per-core sampling, CPU/SoC temperatures, GPU core rail power and system RAM.
- Settings web app, configuration and payload startup/watchdog.

### Additions from the previous project

The original repository's `src/` and `include/` implementation is retained as a telemetry reference. Its loader, injector, daemon and HUD renderer are **not** part of the new payload architecture.

Only missing telemetry capabilities are being transferred into the upstream sampling and scene pipeline:

- **VRAM usage:** sampled independently with `get_page_table_stats(VM_SYSTEM, PAGE_TABLE_VRAM, ...)`. It is displayed separately from system RAM.
- **Fan duty:** sampled through the optional `sceKernelGetCurrentFanDuty` symbol. The raw 0–1024 duty value is converted to a percentage.

The new readings have independent validity flags. An unavailable or invalid reading is rendered as a dash rather than a false zero.

## Telemetry interpretation and limitations

- The upstream CPU per-core calculation is retained; the previous project's aggregate-load approximation is not used.
- The temperature shown in the GPU column is the upstream SoC sensor 0 reading. It is not claimed to be a separately verified GPU-die sensor.
- GPU power is the upstream GPU core rail reading. It is not total APU/SoC power.
- VRAM page-table type 2 and its reported units still require validation on the target console.
- Fan duty depends on the optional firmware symbol being available.
- No GPU busy/utilization counter is claimed.

**Hardware validation is still required.** The current target for console validation is PS5 firmware 13.60. A successful cross-build does not establish that every metric is supported or correctly interpreted on that firmware.

## Build and test

Requirements for the PS5 payload build:

- Full PS5 Payload SDK (the CI currently uses v0.43).
- The SDK's `prospero-cmake` and `prospero-clang` toolchain.
- CMake, Ninja, Python 3 and standard build utilities.

Set the SDK path, then run the upstream build script:

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

# Host-only tests for the scene renderer and settings web helpers
bash upstream/fps-monitor-panel/scripts/test.sh

# Build the PS5 payload
bash upstream/fps-monitor-panel/scripts/build.sh --release --jobs 8
```

The output is:

```text
upstream/fps-monitor-panel/build/bin/fps-monitor.elf
```

GitHub Actions runs the host tests and PS5 SDK build on the development branch. The resulting ELF is uploaded as a workflow artifact. Releases are created only for version tags or an explicit manual release request.

## Licensing and attribution

This project adopts **GPL-3.0-or-later** for the integrated upstream foundation. The upstream license and third-party notices are retained. Original copyright and license notices in imported files must remain intact.

See:

- [LICENSE](LICENSE)
- [Upstream third-party notices](upstream/fps-monitor-panel/THIRD_PARTY_NOTICES.md)
- [Upstream project](https://github.com/rawalakshay/fps-monitor-panel)

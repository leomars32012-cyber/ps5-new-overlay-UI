# PS5 FPS Monitor Overlay

A native in-game performance HUD for jailbroken PlayStation 5 consoles. The shipped payload is based on **[rawalakshay/fps-monitor-panel](https://github.com/rawalakshay/fps-monitor-panel)**. The upstream implementation is the active product foundation; this repository retains its source, build system, settings app, license and notices under `upstream/fps-monitor-panel/`.

> **Current development branch:** `refactor/reference-monitor-integration`  
> **Current validation target:** PS5 firmware 13.60. A successful cross-build and passing host tests do not, by themselves, prove that every firmware-dependent sensor is available or correctly interpreted on hardware.

## Current feature set

### FPS and frame-time monitoring

The upstream monitoring pipeline provides:
- Live FPS and current frame time.
- Ten-second AVG and LOW statistics, with the LOW label reflecting whether it is derived from per-frame data.
- FPS target inference/tracking and good/warning/bad performance states.
- FPS and frame-time history charts, a stability indicator, session statistics, and hitch/stall detection.
- A 7.5-second frame-time graph.
- Detail modes: `minimal`, `compact`, `panel`, and `full`.
- Themes: `classic`, `mono`, `aurora`, and `vice`.

### System telemetry

The ShellUI sampler independently tracks CPU load and temperature, SoC temperature, GPU core-rail power, system RAM, VRAM, fan duty, CPU/GPU clocks, M.2 SSD temperature, SoC power and the console IPv4 address.

| Reading | Implementation / meaning | Important qualification |
|---|---|---|
| CPU load | Per-core sampling; displayed aggregate is the mean across the sampled cores | Retains the upstream per-core calculation |
| CPU temperature | `sceKernelGetCpuTemperature` | Firmware/API dependent |
| GPU temperature | SoC sensor channel 0 | This is an APU/SoC sensor reading, not a separately verified GPU-die sensor |
| GPU power | GPU core rail from `sceKernelGetSocPowerConsumption` | Power-based proxy; there is no GPU busy/utilization counter claimed here |
| System RAM | System page-table statistics | Separate from VRAM |
| VRAM | `get_page_table_stats` with the VRAM page-table selector | Page-table type/units still require target-console validation |
| Fan duty | Optional `sceKernelGetCurrentFanDuty`, converted from 0–1024 to percent | Hidden/unavailable when the optional symbol or valid reading is unavailable |
| CPU clock | Highest reported core clock, with kernel-frequency fallback | Firmware/API dependent |
| GPU clock | SoC GFXCLK domain 20 | Firmware/API dependent |
| SSD temperature | SoC sensor channel 2 (M.2 NVMe) | Firmware/API dependent |
| SoC power | Sum of the eight reported power-rail readings | Distinct from the GPU core-rail reading; decoder requires hardware validation |
| IP address | Console IPv4 from the network helper | Visibility is configurable |

Each reading carries independent validity state. Failed, unavailable or out-of-range readings are represented as unavailable (a dash), rather than being displayed as a misleading zero. A failure in one source does not stop other telemetry sources from updating.

### Overlay settings and live reload

The settings UI controls overlay enablement, edge, alignment, margin, detail level, theme, text size, background opacity, metric visibility, frame-graph visibility/opacity and the extra FPS pill. Settings are stored in the upstream INI configuration.

The ShellUI side checks for configuration changes about once per second and applies updates without requiring the payload to be restarted. If the config file is temporarily missing, the last loaded settings remain active.

### Reorderable system metrics

The settings page includes a **Metric order** control with up/down actions for all nine independent system metrics:

1. CPU — `cpu`
2. GPU — `gpu`
3. Memory — `memory`
4. Fan speed — `fan`
5. IP address — `ip`
6. CPU clock — `cpu_clock`
7. GPU clock — `gpu_clock`
8. SSD temperature — `ssd_temp`
9. SoC power — `soc_power`

The order is independent of each metric's visibility. Hidden metrics stay in the saved sequence and return to their chosen position when made visible again. The scene builder applies this same order in both the compact strip and panel system-metric layouts.

**What is not part of this reorder control:** FPS/chart components and the internal layout groups are separate scene elements. The older six-slot `overlay.order` setting is also distinct; it is not the nine-item standalone monitor `metrics_order` sequence.

The order is serialized as `overlay.metrics_order` in `config.ini`, and is persisted with the other settings. The web settings API accepts the complete nine-item sequence only: missing IDs, duplicates, unknown names and malformed sequences are rejected. The scene-side normalizer also removes invalid/duplicate IDs and appends any missing IDs defensively.

Default:
```ini
metrics_order=cpu,gpu,memory,fan,ip,cpu_clock,gpu_clock,ssd_temp,soc_power
```

Example (reverse sequence):
```ini
metrics_order=soc_power,ssd_temp,gpu_clock,cpu_clock,ip,fan,memory,gpu,cpu
```

Reordering is a draft until **Save** is pressed. **Discard** restores the last saved order; the reset action restores defaults while retaining the master enabled state.

## Architecture and source map

The shipped artifact is `fps-monitor.elf`. It contains two cooperating parts:

1. **Payload/host module** (`source/fps_monitor/source/`): payload entry point, FPS sampling support, settings loading, web server/config API, ShellUI process discovery, injection and watchdog/recovery.
2. **Embedded ShellUI module** (`source/fps_monitor/shellui/` plus `source/shellui/`): injected into `SceShellUI`; reads system telemetry, builds overlay scenes and renders them using the system UI2 widgets. Nothing in the active design requires rendering widgets inside the game process.

The imported implementation is pinned by `upstream/FPS_MONITOR_PANEL_UPSTREAM_COMMIT`. The former top-level `src/` and `include/` code is retained as a historical/reference implementation; its separate loader, injector, daemon and HUD renderer are **not** the active build architecture.

### Important implementation files

| File | Responsibility |
|---|---|
| `source/fps_monitor/source/main.cpp` | Payload entry point, settings policy, ShellUI readiness checks, injection supervision and watchdog |
| `source/fps_monitor/shellui/main.cpp` | ShellUI module entry point, Mono/ShellUI symbol resolution, game container discovery, render-hook installation and settings reload |
| `source/fps_monitor/shellui/monitor_overlay.cpp` | FPS/system sampling, `sample_system()`, `scene_render()`, `OnRender_Hook()` |
| `source/fps_monitor/source/web_config.hpp` | Settings API schema/helpers, metric-order validation, request parsing and INI updates |
| `source/fps_monitor/web/index.html` | Settings web UI, live preview, visibility switches, draft/save/discard, metric ordering |
| `source/libonion_settings/include/onion/settings.hpp` | Settings structure, stable metric IDs, default order, normalization and serialization helper declarations |
| `source/libonion_settings/source/settings.cpp` | INI parsing, `parse_overlay_system_order()`, settings load/save and serialization |
| `source/shellui/include/overlay_scene.hpp` | Telemetry/readings types, FPS statistics, scene model, metric visibility and ordering, `build_scene()` |
| `source/shellui/src/overlay_scene_renderer.cpp` | Reconciles scene rectangles/text into ShellUI UI2 widgets; `overlay_scene_apply()` and cleanup |
| `tests/host/scene_test.cpp` | Host-side scene geometry, visibility and metric-order tests |
| `tests/host/web_config_test.cpp` | Host-side settings API validation, parsing and JSON/config tests |

### Key functions and data flow

**Configuration and persistence**
- `fpsmon::web::valid_system_order()` checks that the supplied order contains each of the nine known metric IDs exactly once.
- `fpsmon::web::valid_value()` applies the setting-specific validation rules.
- `fpsmon::web::parse_updates()` parses and validates form-encoded updates.
- `fpsmon::web::apply_updates()` applies accepted values to the `[overlay]` section while preserving unrelated configuration lines/sections.
- `onion::parse_overlay_system_order()` parses the persisted comma-separated order into stable metric IDs and normalizes it.
- `onion::overlay_normalize_system_order()` drops invalid/duplicate IDs and appends missing IDs.
- `onion::overlay_system_order_name()` serializes the normalized order as comma-separated names.
- `onion::settings_load()`, `onion::settings_load_file()`, `onion::settings_save_file()`, `onion::settings_serialize()` and `onion::settings_save()` handle configuration load, serialization and persistence.

**Web settings UI**
- `api()` communicates with the local `/api/config` endpoint.
- `load()` retrieves current settings from the console.
- `normalizedOrder()` makes the browser-side order complete and unique.
- `moveMetric()` moves one metric up/down in the draft sequence.
- `renderMetricOrder()` renders the reorder controls and the visible/hidden state.
- The Save action posts changed settings; Discard restores the saved snapshot. The master enable switch is applied immediately.

**Sampling and rendering**
- `sample_system()` obtains independent hardware/network readings and their validity flags.
- `scene_render()` maps `onion::Settings` to `scene::Options`, then calls `scene::build_scene()` and `overlay_scene_apply()`.
- `scene::build_scene()` generates the declarative scene and applies the normalized system-metric order to compact and panel layouts.
- `overlay_scene_apply()` reconciles the scene into UI2 widgets; `overlay_scene_remove_all()` removes cached widgets and releases their GC handles.
- `OnRender_Hook()` drives periodic sampling/render work and calls `overlay_reload_settings()`.
- `overlay_reload_settings()` reloads settings when the configuration file's modification time changes; `load_settings()` applies the new settings and layout.

## Build and tests

### Requirements
- A full PS5 Payload SDK (the CI workflow currently downloads SDK v0.43).
- `prospero-cmake` and the SDK's Prospero compiler/linker toolchain.
- CMake, Ninja, Python 3 and a C++20 host compiler.

### Host tests (no PS5 SDK required)
```bash
# Run the settings-web helper tests and scene/layout tests
bash upstream/fps-monitor-panel/scripts/test.sh
```

The script compiles with C++20, `-Wall` and `-Werror`. It runs:
- `tests/host/web_config_test.cpp` for validation, URL/form decoding, update parsing, configuration updates and JSON serialization.
- `tests/host/scene_test.cpp` for scene helpers, metric visibility/order, layouts and generated render cases.

Optional SVG/PNG render output (requires `rsvg-convert`):
```bash
bash upstream/fps-monitor-panel/scripts/test.sh --render
```

### PS5 payload build
```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

# Release build; output goes to upstream/fps-monitor-panel/build/bin/
bash upstream/fps-monitor-panel/scripts/build.sh --release --jobs 8
```

Other supported script options include `--debug`, `--jobs N` and `--build-dir DIR`. The output is:
```text
upstream/fps-monitor-panel/build/bin/fps-monitor.elf
```

GitHub Actions runs the host tests and PS5 SDK build and uploads the build artifacts. A release is published for a `v*` tag or when explicitly requested through the workflow's manual release input. Check the workflow file for the exact branch/tag triggers and artifact names.

## Validation status and known limitations

- Host tests cover the portable settings and scene-building logic; they do not execute ShellUI or firmware kernel APIs.
- A successful PS5 cross-build proves compilation/linking, not that a metric's kernel symbol, sensor selector, units or interpretation is valid on every firmware.
- GPU temperature is the SoC sensor-0 value currently displayed in the GPU section.
- GPU power is the GPU core rail and must not be described as GPU utilization.
- SoC power is a separate sum across eight rails. The raw layout is community-sourced and needs console-side verification.
- VRAM page-table selector/type and units need hardware confirmation.
- Fan duty depends on resolution of `sceKernelGetCurrentFanDuty`.
- The settings page/API is intended for the local network and has no password authentication; do not expose its port to an untrusted network.

## Attribution and licensing

This repository adopts **GPL-3.0-or-later** for the integrated upstream foundation. Preserve original copyright and license notices in imported source files.

- [Project license](LICENSE)
- [Upstream license](upstream/fps-monitor-panel/LICENSE)
- [Upstream third-party notices](upstream/fps-monitor-panel/THIRD_PARTY_NOTICES.md)
- [Original upstream project](https://github.com/rawalakshay/fps-monitor-panel)

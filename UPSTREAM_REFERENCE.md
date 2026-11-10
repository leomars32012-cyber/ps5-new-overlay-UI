# Upstream reference and integration notes

This project incorporates and adapts code from:

- https://github.com/rawalakshay/fps-monitor-panel
- Upstream branch: `main`
- Imported reference snapshot inspected: 2026-11-10
- Upstream project license: GNU GPL-3.0

The reference overlay implementation and its required support code remain
subject to their original copyright notices and license terms. Preserve
file-level headers. Do not remove or rewrite upstream attribution.

The integrated project is intended to retain the upstream ShellUI overlay
architecture (FPS timeline, detail layouts, scene renderer, and telemetry
presentation) while integrating the existing PS5 telemetry collectors.

This repository also contains independently developed code from its original
author. The project is distributed as a whole under GPL-3.0 where applicable;
third-party components may retain their own additional license obligations.
See `THIRD_PARTY_NOTICES.md`.

## Telemetry integration goals

- FPS, frame time, averages, lows, hitches, and target tracking from upstream.
- CPU temperature and CPU aggregate/per-core load.
- SoC temperature and GPU rail power (distinct telemetry concepts).
- RAM and VRAM page-table readings, with validity indicators.
- Fan duty, separately from GPU/SoC power.
- Unavailable or unverified readings are rendered as unavailable, never
  silently converted to a genuine zero.

Hardware API semantics, VRAM accounting, and power-rail labels must be
validated on the target PS5 firmware before being presented as authoritative.

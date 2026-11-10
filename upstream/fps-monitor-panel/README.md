<h1 align="center">fps-monitor-panel</h1>

<p align="center">
  An in-game performance overlay for jailbroken PS5 consoles.<br>
  Frame rate, frame times, CPU / GPU temperatures and memory, drawn over any game from a single payload.
</p>

<p align="center">
  <a href="https://github.com/rawalakshay/fps-monitor-panel/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/rawalakshay/fps-monitor-panel?style=flat-square&color=5aa9e6"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0" src="https://img.shields.io/github/license/rawalakshay/fps-monitor-panel?style=flat-square&color=5aa9e6"></a>
  <img alt="Platform: PS5" src="https://img.shields.io/badge/platform-PS5-5aa9e6?style=flat-square">
  <a href="https://github.com/rawalakshay/fps-monitor-panel/stargazers"><img alt="GitHub stars" src="https://img.shields.io/github/stars/rawalakshay/fps-monitor-panel?style=flat-square&color=f5b13d"></a>
  <a href="https://ko-fi.com/rawalakshay"><img alt="Support on Ko-fi" src="https://img.shields.io/badge/Ko--fi-support-ff5e5b?style=flat-square&logo=ko-fi&logoColor=white"></a>
</p>

<p align="center">
  <a href="#install">Install</a> ·
  <a href="#settings-web-app">Web app</a> ·
  <a href="#configuration">Configuration</a> ·
  <a href="#troubleshooting">Troubleshooting</a> ·
  <a href="#building-from-source">Build</a> ·
  <a href="#credits">Credits</a>
</p>

![fps-monitor panel and frame-time graph over a Tekken 8 fight at a locked 60 FPS](docs/screenshots/tekken8-fight.jpg)

## Highlights

- **Live frame rate** with AVG and LOW over the last 10 s, and session totals since the game started.
- **Charts**: a 10 s FPS chart, a frame-time chart, a stability bar, and a 7.5 s frame-time graph along the bottom of the screen.
- **Hitch detection**: stalls longer than 100 ms are flagged in red with their length.
- **System stats**: CPU load and temperature, GPU temperature and power draw, memory in use.
- **PS5 and PS4 games**: PS4 titles running in backward compatibility are measured too.
- **Nothing injected into the game**: the overlay is drawn by the system UI.
- **Configurable**: four detail levels, four colour themes, position, size and opacity.
- **Settings from your phone**: a built-in web app turns the overlay on or off and changes every setting live, no reboot.
- **Survives rest mode**: the overlay reloads itself after resume.

## Screenshots

Captured on a PS5 with the default settings.

| Tekken 8 menu (PS5) | Red Dead Redemption 2 (PS4) | Tekken 8 cutscene (PS5) |
| :---: | :---: | :---: |
| ![Panel at a 60 FPS target in the Tekken 8 menu with two short drops shown as amber bars](docs/screenshots/tekken8-menu.jpg) | ![Panel on a PS4 title at its 30 FPS target with slow stretches shown in amber](docs/screenshots/rdr2-ps4.jpg) | ![Cutscene with frame-time spikes up to 185 ms printed in red in the bottom graph](docs/screenshots/tekken8-cutscene.jpg) |
| Short drops show as amber bars | PS4 games, 30 FPS target detected | Stalls print their length in red |

## The overlay at a glance

Layout illustrations; the numbers are examples.

<div align="center">
<table>
  <tr>
    <th>Panel, on target</th>
    <th>Panel, during a drop</th>
  </tr>
  <tr>
    <td><img src="docs/ui/ui-panel.png" alt="Panel at a locked 60 FPS: PS5 and TARGET 60 pills, AVG and LOW over the last 10 s, frame time, session row, FPS and frame-time charts, stability bar, and the CPU, GPU and memory row" width="340"></td>
    <td><img src="docs/ui/ui-panel-drop.png" alt="The same panel during a drop: 44 FPS in red, a HITCH 240 ms badge, red and amber bars, and 85 % on target" width="340"></td>
  </tr>
</table>
</div>

**Frame-time graph** (on by default): 7.5 s of frame times centred along the bottom edge.

<img src="docs/ui/ui-graph.png" alt="Frame-time graph with MAX 240.0 ms and one 240 ms stall labelled in red" width="100%">

**Compact strip** (`detail=compact`): the key numbers in one row.

<img src="docs/ui/ui-strip.png" alt="Compact strip: 60 FPS, frame time, AVG, LOW, MAX frame time, a mini frame-time chart, CPU and GPU" width="100%">

**Themes** (`theme=`): classic, mono, aurora and vice.

<img src="docs/ui/ui-themes.png" alt="Four theme swatches: classic blue, mono white, aurora cyan with an edge bar, vice pink with a pink and orange top bar" width="100%">

### Reading the panel

| Label | Meaning |
| --- | --- |
| **FPS** | Frame rate over the last 0.5 s. White on target, amber below 95 % of it, red below 80 %. |
| **AVG / LOW** | Average frame rate and the rate of the slowest 1 % of frames, over the last 10 s. |
| **TARGET** | The frame-rate cap the game appears to run at (30, 40, 60 or 120), detected automatically. |
| **FRAME** | Current frame time. A red **HITCH** badge shows the length of any stall longer than 100 ms in the last second. |
| **SESSION** | Time since the game started, with its average, low, share of time below target, and number of drops. |
| **STABILITY** | The last 10 s split into on target, slow (80–95 %) and dropped (below 80 %). |

## Install

**Requirements:** a jailbroken PS5 that runs payloads through an ELF loader, and a way to send an ELF to it over the network (for example `nc` or any payload sender).

1. Download the latest `fps-monitor` ELF from [Releases](https://github.com/rawalakshay/fps-monitor-panel/releases/latest).
2. Send it to the console's ELF loader. A `[FPS Monitor] started` notification confirms it is running, followed by `[FPS Monitor] Settings: http://<ps5-ip>:1994` with the address of the [settings web app](#settings-web-app).
3. Start a game. The panel appears in the top-left corner.

> [!NOTE]
> The payload runs until the console is turned off. It reloads the overlay by itself after rest mode; after a full reboot, send it again. Sending it twice shows `[FPS Monitor] already running`.
>
> The overlay waits for the system UI to settle before it loads. When the payload starts with the console (autoload at boot), the overlay appears about two minutes after boot; sent later, it appears about 10 seconds after the payload starts.

> [!WARNING]
> Do not run it alongside another performance overlay, such as the monitor bar of a full homebrew toolkit. It cannot detect other overlays, and both would draw at once.

## Settings web app

The payload serves a small settings page on port **1994**. Open it from the browser on your phone (or a PC) to switch the overlay on or off and change its look while you play. Nothing to install: it is a normal web page served by the console.

<div align="center">
<table>
  <tr>
    <th>On a phone</th>
    <th>Settings</th>
    <th>Light mode</th>
  </tr>
  <tr>
    <td><img src="docs/ui/web-app-phone.png" alt="Web app on a phone: console address and Connected status, the FPS overlay on/off switch, a live preview of the overlay on a sample game screen, and the Position settings" width="250"></td>
    <td><img src="docs/ui/web-app-phone-settings.png" alt="Web app settings on a phone: detail level, colour theme swatches, text size, background opacity, and the system row and frame-time graph switches" width="250"></td>
    <td><img src="docs/ui/web-app-phone-light.png" alt="The same phone page in light mode" width="250"></td>
  </tr>
</table>
</div>

On a wider screen the preview stays on the left and the settings sit on the right. The page follows the phone or PC's light or dark mode.

<img src="docs/ui/web-app-desktop.png" alt="Web app on a PC: the on/off switch and preview on the left, Position and Look settings on the right, and the Save bar along the bottom" width="100%">

### Open it on your phone

1. **Connect your phone to the same network as the PS5** (the same home Wi-Fi or router). Mobile data, a guest Wi-Fi network or a VPN will not reach the console.
2. **Find the console's address.** It is in the notification shown when the payload starts: `[FPS Monitor] Settings: http://192.168.1.42:1994`. You can also look it up on the PS5 under **Settings › Network › Connection Status › View Connection Status** (IPv4 Address).
3. **Type the address into the phone's browser** (Safari, Chrome or any other), including `http://` and `:1994`, for example `http://192.168.1.42:1994`. Use `http`, not `https`. The browser may mark the page "Not secure"; that is normal for a page on your home network.
4. **Optional: add it to your home screen** so it opens like an app next time.
   - iPhone (Safari): tap **Share**, then **Add to Home Screen**.
   - Android (Chrome): tap the **⋮** menu, then **Add to Home screen**.

The page only loads while the payload is running. After a reboot, send the payload again first.

### Using it

- **FPS overlay switch** (top of the page): turns the whole overlay on or off. It applies right away; there is no Save step.
- **Preview**: a sketch of where the overlay sits on screen and how it looks with your unsaved choices. The numbers in it are examples, not live readings.
- **Settings**, in three groups:
  - **Position**: screen edge, alignment along that edge, and margin.
  - **Look**: detail level, colour theme, text size and background opacity.
  - **Panel contents**: system row, frame-time graph (and its opacity), and the extra FPS pill.
- **Save** (bottom bar): applies your changes. The overlay updates within about a second, even mid-game. A dot next to a setting marks a change that is not saved yet.
- **Discard** throws away unsaved changes. **Reset all to defaults**, under **config.ini**, restores the default values (press Save to apply them).
- **config.ini** shows the exact `[overlay]` lines the page writes.

Changes are saved to `/data/fps-monitor/config.ini`. The file is created if it is missing; only the keys you change are rewritten, and your other lines and comments are kept. The same settings can be edited by hand, see [Configuration](#configuration).

> [!NOTE]
> The page has no password: anyone on your local network can open it and change the overlay settings. It only changes overlay settings; it cannot reach anything else on the console.

## Configuration

Configuration is optional. Without a config file, every setting uses its default. The easiest way to change settings is the [settings web app](#settings-web-app); this section is for editing the file directly.

### Editing the file by hand

1. Copy [`config.ini.example`](config.ini.example) to `/data/fps-monitor/config.ini` on the console. The folder is created on first run.
2. Keep the `[meta]` section: a file without `schema_version=1` is ignored.
3. Edit the `[overlay]` keys.

The overlay picks up changes within about a second. Invalid values fall back to the default.

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `enabled` | `true`, `false` | `true` | Show or hide the whole overlay. |
| `detail` | `minimal`, `compact`, `panel`, `full` | `panel` | `minimal` is an FPS pill, `compact` a single strip, `panel` the full corner panel. `full` is currently the same as `panel`. |
| `theme` | `classic`, `mono`, `aurora`, `vice` | `classic` | Colour palette. |
| `edge` | `top`, `bottom` | `top` | Screen edge the overlay sits on. |
| `align` | `left`, `center`, `right` | `left` | Position along that edge. |
| `font_size` | `small`, `medium`, `large` | `medium` | Text size; the whole overlay scales with it. |
| `opacity` | `0`, `40`, `65`, `85` | `85` | Background opacity in percent; `0` hides the background. |
| `margin` | `0`, `5`, `10`, `20` | `20` | Gap from the screen edges, in pixels. |
| `system` | `true`, `false` | `true` | CPU / GPU / memory row. |
| `frame_graph` | `true`, `false` | `true` | 7.5 s frame-time graph along the bottom edge (top edge when `edge=bottom`). |
| `frame_graph_opacity` | `0`, `40`, `65`, `85` | `85` | Background opacity of the frame-time graph. |
| `fps_pill` | `true`, `false` | `false` | Also show a small FPS pill in the opposite corner. |
| `brand` | `true`, `false` | `false` | Show a "4aLse Benchmarks" header on the panel. File only; not in the web app. |

Example:

```ini
[meta]
schema_version=1

[overlay]
detail=compact
edge=bottom
align=right
theme=aurora
frame_graph=false
```

## Troubleshooting

The log is at `/data/fps-monitor/fps-monitor.log`.

| Notification | Meaning |
| --- | --- |
| `[FPS Monitor] started` | The payload is running and will load the overlay. |
| `[FPS Monitor] Settings: http://<ps5-ip>:1994` | The settings web app is ready at that address. |
| `[FPS Monitor] Settings web app on port 1994` | The web app is ready, but the console's address could not be read yet. Look it up under **Settings › Network › Connection Status**. |
| `[FPS Monitor] Settings web app: port 1994 busy, retrying` | Something else is using port 1994. The overlay still works; the web app keeps trying to start. |
| `[FPS Monitor] already running` | A copy is already running; the new one exited. |
| `[FPS Monitor] overlay failed to load` | The overlay could not be loaded into the system UI. The payload tries 3 times, then waits for the system UI to restart (reboot, or rest mode and back). Check the log. |

- **"NO GAME DETECTED"**: no frames are being measured. This is normal in menus and loading screens.
- **Settings are ignored**: check the path, the `[meta]` section and the values against the table above.
- **The web app does not open on the phone**:
  - Check that the payload is running (it must be sent again after every reboot).
  - Check that the phone is on the same Wi-Fi as the PS5, not on mobile data, a guest network or a VPN. Some routers block devices from talking to each other ("AP isolation" or "client isolation"); turn that off in the router settings.
  - Check the address, including `http://` and `:1994`. The console's address can change after a router restart; check it again in the PS5 network settings. A fixed address (DHCP reservation) in your router stops it changing.
- **The page says "Can't reach the FPS Monitor on this console"**: the page loaded earlier but the payload is no longer running, or the console is in rest mode. Wake the console, send the payload again if needed, then tap **Retry**.
- **A change does not show in the game**: wait a second after Save, then check the log for `settings reloaded`.
- **Two overlays on screen**: another performance overlay is running. Use only one.

## How it works

- **PS5 games**: the payload reads the display scanout counters (`/dev/dce`) and the frame counters of the game's own graphics driver (`libSceAgcDriver.sprx`) from outside the game process.
- **PS4 games**: it places a small remote hook on the game's GNM submit-and-flip call to count presented frames.
- **Drawing**: a module loaded into SceShellUI (the system UI) draws the overlay with the system's own UI widgets. Nothing is loaded into the game.

## Building from source

Requirements: Linux or WSL, the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) (a full release that includes `target/lib/libc++.a`), `cmake`, `ninja` and `python3`.

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
./scripts/build.sh          # --debug, --release, --jobs N, --build-dir DIR
```

The output is `build/bin/fps-monitor.elf`, the only file to send. The system UI module is built first and embedded in it.

Layout tests run on the host, without a console:

```sh
./scripts/test.sh           # add --render to write PNG previews
```

<details>
<summary>Project layout</summary>

| Path | Contents |
| --- | --- |
| `source/fps_monitor/source/` | Host payload: FPS sampler threads, the system UI injector / watchdog and the settings web server. |
| `source/fps_monitor/web/` | Settings web app page (`index.html`), embedded in the payload. |
| `source/fps_monitor/shellui/` | Overlay module entry point and `monitor_overlay.cpp` (sampling, render loop). |
| `source/shellui/` | UI helpers, scene layout (`overlay_scene.hpp`) and renderer. |
| `source/daemon/` | FPS sampler (`daemon_fps.cpp`): scanout, AGC counters, PS4 GNM hook. |
| `source/libonion_*`, `libhijacker`, `libNineS` | Vendored support libraries: process access, injection, settings, logging, notifications. |
| `source/i18n/` | Notification strings. |
| `source/platform/ps5/stubs/` | PS5 system library link stubs. |
| `tests/host/` | Host-side layout tests and renders. |
| `third_party/` | cJSON, SHA-1. |
| `docs/` | README screenshots and illustrations. |

</details>

## Credits

- Based on **HyperHEN** by **LightningMods**: the overlay, FPS sampler and support libraries come from it.
- Overlay design from **onionHEN 0.9.0** by **LightningMods**.
- FPS sampling approach follows **PHU Games Tools** by [ArkSama](https://github.com/ArkSama).
- ELF loading and ptrace helpers by **John Törnblom** (GPL-3.0-or-later).
- **cJSON** by Dave Gamble and contributors (MIT), **SHA-1 in C** by Steve Reid (public domain), **inih** by Ben Hoyt (BSD-3-Clause), **Hacker Disassembler Engine 64** by Vyacheslav Patkov.

Full notices are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## License

GPL-3.0. See [`LICENSE`](LICENSE). Third-party components keep their own terms.

<p align="center">
  If fps-monitor-panel is useful to you, a ⭐ on the repo helps others find it.<br>
  You can also support development on <a href="https://ko-fi.com/rawalakshay">Ko-fi</a>.
</p>

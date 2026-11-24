# Pokémon Black 2 — PlayStation Vita Port (Beta)

A native PlayStation Vita port of the **Pokémon Black 2** decompilation/PC port ([fuddlesworth/pokebw2](https://github.com/fuddlesworth/pokebw2)), built with **VitaSDK** and **vitaGL**.

> **Disclaimer:** This repository and its releases contain **no copyrighted game assets, ROMs, or Nintendo binaries**. You must supply your own extracted files from a legally owned copy of *Pokémon Black 2*.

---

## Features

- **Native ARM Cortex-A9 + vitaGL Engine:** Runs at up to **60 FPS** on PS Vita (444 MHz ARM / 222 MHz BUS / 222 MHz GPU / 166 MHz XBAR) using dual-core 2D compositing (Cores 1 & 3) and hardware-accelerated 3D rendering.
- **Independent Edition Selection:**
  - **Original Edition:** Preserves authentic Pokémon Black 2 pacing, battle animations, text speed, and mechanics.
  - **Enhanced Edition:** Optional Vita QoL enhancements (2× / Skip battle animations, Fast / Instant text, Auto-Run, Faster Menus, Enhanced Touch + Right-Stick virtual touch cursor) while keeping original battle math, encounters, RNG, and progression untouched.
- **3 Runtime Screen Layouts (switchable anytime without restarting):**
  - **Single Screen (16:9 Widescreen or 4:3):** True 16:9 horizontal 3D FOV expansion (`960×544`) with frustum-culling adjustment for overworld, battles, and 3D cutscenes, paired with a **Kingdom Hearts 358/2 Days-style 4-Corner Picture-in-Picture (PiP)** bottom screen (configurable corner, size, opacity, Auto, or Off). Title screen, Starter selection, and Name entry automatically frame both screens cleanly.
  - **Hybrid:** Prominent 4:3 primary screen (`725×544` or `680×510`) with a configurable secondary window.
  - **Side-by-Side:** Both DS screens side-by-side in exact 4:3 (`480×360` each).
- **Internal 3D Resolution Scaling:**
  - **Native (1×):** `338×192` (16:9) / `256×192` (4:3)
  - **2× Resolution:** `677×384` (16:9) / `512×384` (4:3) — recommended for locked 60 FPS
  - **Vita HD / 4× (Experimental):** `960×544` (16:9 native 1:1 OLED) / `725×544` (4:3) — PowerVR SGX543 tile-aligned with automatic CDRAM fallback guardrails
- **Real-Time Core 2 Audio Engine:** Dedicated Core 2 48 kHz synthesizer with triple-buffered 64-byte-aligned `sceAudioOut` DMA, linear resampling, and zero ring-buffer underruns.
- **Native PS Vita OS Keyboard (`SceImeDialog`):** Automatically opens the Vita system keyboard for player, rival, and Pokémon naming prompts (with seamless DS keyboard fallback on cancel).
- **Native Save Support & Backups:** Saves directly to `ux0:data/pokebw2/black2.sav` (512 KiB flash format) with one-click backup/restore inside the overlay menu.
- **Native PS Vita Overlay Menu (`L + R + Start`):** Clean translucent glass UI with Display, Gameplay, Controls, and System tabs, live FPS/RAM/CDRAM telemetry, and persistent settings (`ux0:data/pokebw2/vita_settings.cfg`).

---

## Installation

1. **Install the VPK:**
   Download `pokebw2-vita-v0.1-beta.vpk` from the [Releases](../../releases) page and install it on your PS Vita via **VitaShell**.
2. **Extract Your Own Pokémon Black 2 Assets:**
   Extract the unpacked ROM filesystem (`a/`, `swan_sound_data.sdat`, etc.) from your own legal copy of *Pokémon Black 2* (USA).
3. **Copy Assets to Your PS Vita:**
   Place the extracted game files into:
   ```
   ux0:data/pokebw2/files/
   ```
   Verify that at minimum these paths exist:
   - `ux0:data/pokebw2/files/a/0/0/2`
   - `ux0:data/pokebw2/files/swan_sound_data.sdat`
4. **Launch:** Open the **Pokemon Black 2** bubble from LiveArea!

Save data, settings, and diagnostic logs are stored in `ux0:data/pokebw2/`:
- `black2.sav` (game save, plus `black2.sav.bak` when backed up via overlay)
- `vita_settings.cfg` (persistent port configuration)
- `log.txt` (runtime diagnostics)

---

## Controls

| Input | Action |
|---|---|
| **D-Pad / Left Stick** | Move / Navigate menus |
| **Cross / Circle** | A / B *(swappable between Vita and Nintendo presets in Overlay → Controls)* |
| **Triangle / Square** | X (Field Menu) / Y |
| **L / R** | DS L / R |
| **Start / Select** | DS Start / Select |
| **Touchscreen** | Direct DS bottom-screen touch (and tap-to-peek PiP in Single Screen) |
| **Right Stick + R** | Virtual touch cursor *(Enhanced Edition)* |
| **L + R + Start** | Open / Close **PS Vita Overlay Menu** *(combo configurable)* |
| **Select + R** | Cycle **KH 4-Corner PiP position** (in Single Screen) or Screen Layout |
| **Select + L** | Capture `960×544` screenshot to `ux0:data/pokebw2/snap_960x544.bmp` |

---

## Building from Source

### Prerequisites
- [VitaSDK](https://vitasdk.org/) installed with `$VITASDK` in `PATH`
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) & `mathneon` installed into `$VITASDK/arm-vita-eabi`
- CMake ≥ 3.20 and Ninja

### Build Commands
```bash
export VITASDK=~/vitasdk
export PATH=$VITASDK/bin:$PATH

cmake -S vita -B build-vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-vita -j$(nproc)
```
Output files:
- `build-vita/pokebw2.vpk` (installable VPK, Title ID `PKBW20001`)
- `build-vita/eboot.bin` (for fast FTP iteration to `ux0:app/PKBW20001/eboot.bin`)

---

## Beta Status & Known Notes

- **Verified on real PCH-1000 PS Vita (FW 3.65):** Boot, Title screen, Prof. Juniper Intro, Native Vita OS Keyboard naming, Opening cutscene, Aspertia City 16:9 widescreen overworld, Bianca starter selection, Rival battle, X-menu (Pokémon, Options, Bag, Save), Saving & Loading (`black2.sav`), Core 2 audio (`ur=0`), and runtime overlay switching.
- **Beta Caveats:** Full end-to-end playthrough past early Unova is still being tested; wireless/C-Gear network features are stubbed; **Vita HD / 4× (`960×544`)** is experimental and more GPU-intensive than **2× Resolution** in heavy scenes.

---

## Credits

- Upstream decompilation & PC port: [fuddlesworth/pokebw2](https://github.com/fuddlesworth/pokebw2)
- VitaSDK & [vitaGL](https://github.com/Rinnegatamante/vitaGL) by Rinnegatamante and contributors
- ARM7 sequence engine: [libntr](https://github.com/cybervisi0n/libntr)

# RDR2 Mac Frame Gen

Frame generation for **Red Dead Redemption 2** (Windows, DirectX 12) on Apple Silicon Macs.

RDR2 has upscaling (DLSS and FSR 2) but no frame generation. This mod adds it: it takes the depth and motion data the game already gives its upscaler and uses it to generate an extra frame between every two real ones, on Apple's MetalFX frame interpolator. Roughly doubles the frame rate you see.

> **You must download 6 NVIDIA files yourself.** This repo only contains the mod (`version.dll`). The NVIDIA Streamline files it needs aren't included; you get them for free from NVIDIA's official release (step 2 below).

Tested on a MacBook Air M4 (macOS 27, CrossOver with D3DMetal 3 / GPTK 3, 100 Hz display): **about 70 fps with FSR 2 Performance**, looking close to native 1080p. It will probably also work in other Wine-based launchers and wrappers that use D3DMetal with DLSS/MetalFX support, but only CrossOver has been tested.

**Before you start:**
- **Use FSR 2 in the game's graphics settings** (recommended; Performance looks great) or DLSS. With neither on, there's no frame generation.
- **Press Ctrl+Shift+M in the game** to open the mod's menu (frame generation on/off, native resolution). It's an odd combination on purpose: it works on a laptop keyboard and RDR2 doesn't use it.
- **The HUD doesn't ghost.** The minimap, crosshair, ammo and prompts stay sharp while the world gets the generated frames.

> Not affiliated with Rockstar, NVIDIA, AMD, Apple or CodeWeavers. **Story mode only.** Don't use it in Red Dead Online.

## FSR 2 or DLSS?

The mod works with both; pick one in RDR2's graphics settings.

- **FSR 2 (recommended).** AMD's upscaler, built into RDR2 and tuned by Rockstar. It runs as part of the game, so it looks the same as on Windows. Even Performance mode looks close to native.
- **DLSS.** On a Mac, CrossOver turns DLSS into Apple's MetalFX upscaler. It works, but foliage (bushes, grass, trees) flickers and the image is blurrier, even at native resolution.

## The in-game menu: Ctrl+Shift+M

Press **Ctrl+Shift+M** anytime in the game to open or close it.

- **Frame generation:** on or off, takes effect immediately.
- **Native resolution (no upscaling):** the game renders at your full resolution in every FSR 2/DLSS mode, and the upscaler only smooths edges. Sharpest, but much slower (on an M4 Air about 50 fps instead of 70 with FSR 2). **Takes effect after restarting the game**, because RDR2 only asks for its render resolution at startup. Leave it off if you want the speed.

Settings are saved in `framegen.ini` in the game folder and stay between launches.

**While the menu is open, the game still receives your keys and mouse.** Use the mouse to click the checkboxes, and don't press keys the game would act on. Opening the menu while the game is paused works, but be careful with arrow keys and Enter there: they also move through the pause menu.

## Known issues

- **Turn HDR off**, in RDR2's display settings and in macOS (System Settings → Displays). With HDR on, the HUD fix doesn't work and the HUD ghosts again. Restart the game after changing it.
- **Turn VSync off on a 60 Hz display.** With VSync on, a 60 Hz screen (most TVs) gets stuck at 30 fps shown (15 real) and feels sluggish. VSync off has no tearing on a Mac.
- **Keys and mouse reach the game while the menu is open** (see above).
- **Scopes ghost slightly.** When aiming down a scope (rifles, repeaters), the scope overlay can ghost a little when you move. RDR2 seems to draw it as part of the picture rather than with the HUD, so the HUD fix doesn't catch it. It's minor.
- CrossOver logs a few missing NVIDIA functions (`SetReflexSync` and others). These are harmless.

## Requirements

- Apple Silicon Mac
- CrossOver with D3DMetal (or, untested, another Wine wrapper with D3DMetal and DLSS/MetalFX support)
- RDR2 in DirectX 12 mode, with **FSR 2 or DLSS on** in the game's graphics settings
- In the bottle's CrossOver settings: **DLSS / MetalFX enabled** (`D3DM_ENABLE_METALFX=1`). Needed even with FSR 2: the frame generation itself runs on MetalFX.

## Install

1. Download `version.dll` from this repo's [Releases](../../releases).
2. **Download the NVIDIA files yourself:** get **Streamline SDK v2.14.16** from [NVIDIA's Streamline releases](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16). Take the plain `streamline-sdk-v2.14.16.zip`, **not** the `aarch64` or `arm64ec` ones; the game is an x64 Windows program even on a Mac. Unzip it, and from its `bin/x64` folder take these 6 files:
   - `sl.interposer.dll`
   - `sl.common.dll`
   - `sl.dlss_g.dll`
   - `sl.reflex.dll`
   - `sl.pcl.dll`
   - `nvngx_dlssg.dll`
3. Put all 7 files in the RDR2 game folder, next to `RDR2.exe`.
4. Tell Wine to load the mod's `version.dll`: in CrossOver, open the bottle's **Wine Configuration** → **Libraries**, type `version`, click **Add**, and make sure it says **(native, builtin)**.
5. Start the game, set **FSR 2** (or DLSS) in the graphics settings, and play. With the Metal HUD on, you'll see MetalFX frame interpolation listed. Turn VSync and HDR off (see Known issues).

### Why is the mod called `version.dll`?

The game loads a standard Windows file called `version.dll`, and it checks its own folder before the system one. A file with that name next to `RDR2.exe` gets loaded instead, passes everything through to the real one, and runs the mod inside the game. No game files are changed. This is the same trick many mods use (ReShade, OptiScaler); a different name and the game would never load it.

## Turning it off and on

- **Frame generation only:** Ctrl+Shift+M → uncheck **Frame generation**.
- **The whole mod:** move `version.dll` out of the game folder (e.g. into a folder called `framegen-off`). Without it the mod doesn't load and the other 6 files do nothing. Move it back to turn it on.
- **Tip:** after installing, select the 7 files in Finder, right-click, and give them a color tag (e.g. red) so you can always find them among the game's files.

Frame generation always makes one extra frame per real frame (2x). The Metal HUD can briefly show a bit more than 2x because of how it averages; that's normal.

## Uninstall

Delete the 7 files and `framegen.ini`, and remove the `version` entry from Wine Configuration.

**Be careful with the two similar names:**
- `nvngx_dlssg.dll` (**with** a "g") is frame generation, from this install. Delete this one.
- `nvngx_dlss.dll` (**no** "g") is the game's own DLSS. **Don't delete it**, or DLSS stops working.

## Troubleshooting

The mod writes `ngxlog.txt` in the game folder (new each launch, stays small). Its first line shows the mod version. Look for lines starting with `STREAMLINE`, `FSR2`, `HUDLESS`, `SL ERROR` or `EXCEPTION`, and include the file if you report a bug. If the game crashes, the log ends with a "black box" of the last few hundred events before the crash.

Some `SL ERROR` lines are normal under CrossOver and harmless: `setDynamicMFGParams`, `setFlipConfig`, `SetReflexSync`, `NGX Updater not available`, `0xbad00000`, and `ReflexNotDetectedAtRuntime`.

- **No frame generation and no `ngxlog.txt`**: the Wine override from step 4 is missing. It has to be set in Wine Configuration; an environment variable doesn't work.
- **"DLSS-G not supported"** in the log: DLSS/MetalFX isn't enabled in the bottle's settings.
- **Nothing happens**: neither FSR 2 nor DLSS is on in the game, or frame generation is unchecked in the Ctrl+Shift+M menu.
- **Slow in every FSR 2/DLSS mode**: Native resolution is on in the menu. Turn it off and restart the game.

---

## For developers

Players don't need anything below; the download in Releases is ready to use.

### Building

Needs MinGW-w64 (`brew install mingw-w64`), NVIDIA's Streamline headers and Dear ImGui (for the menu):

```bash
git clone --depth 1 --branch v2.14.16 https://github.com/NVIDIA-RTX/Streamline.git streamline/src
```

```bash
git clone --depth 1 --branch v1.91.9 https://github.com/ocornut/imgui.git imgui
```

```bash
./src/build.sh
```

This produces `src/version.dll`.

### How it works

- `src/ngxlog.c` loads as `version.dll` and forwards every call to the real one. It hooks the game's upscaler calls to capture depth, motion vectors and jitter every frame: CrossOver's NGX (DLSS) exports, and AMD's `ffx_fsr2_api_x64.dll` (FSR 2; both its 2.0 and 2.1+ data layouts are handled). It also answers the game's "what resolution should I render at?" question for the native-resolution setting, and hooks DXGI and D3D12 device creation so Streamline can wrap the factory and swapchain (manual hooking, no code patching).
- **HUD fix:** after the upscaler, RDR2 post-processes, writes the finished picture into the backbuffer, and only then binds the backbuffer together with a depth/stencil buffer to draw its HUD. The mod copies the backbuffer at that moment, on the game's own command list, and gives the copy to Streamline as the HUD-less image. D3DMetal generates frames from it and lays the HUD on top unwarped.
- `src/slfg.cpp` runs Streamline: per-frame tokens, camera constants, resource tags (depth, motion vectors, HUD-less color), Reflex markers, and DLSS-G turned on.
- `src/menu.cpp` is the Ctrl+Shift+M menu, drawn with Dear ImGui onto the backbuffer just before it's presented (after the HUD-less copy, so it isn't warped either), plus `framegen.ini`.
- `src/fg.h` is an older path that drives D3DMetal's frame generator directly and presents frames itself. It's kept as a fallback but paces worse.

## License

MIT, see [LICENSE](LICENSE). Fork it, change it, share it. The NVIDIA Streamline files are not included in this repo. They're MIT-licensed by NVIDIA and come from NVIDIA's own release. Dear ImGui (MIT) is compiled into `version.dll`; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

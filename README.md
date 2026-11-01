# RDR2 Mac Frame Gen

Frame generation for **Red Dead Redemption 2** (Windows, DirectX 12) on Apple Silicon Macs.

RDR2 only ships DLSS upscaling, not frame generation. This mod adds the missing piece: it takes the depth and motion data the game already sends to DLSS and uses it for frame generation, which runs on Apple's MetalFX. Roughly doubles the frame rate you see.

> **You must download 6 NVIDIA files yourself.** This repo only contains the mod (`version.dll`). The NVIDIA Streamline files it needs aren't included; you get them for free from NVIDIA's official release (step 2 below).

Tested on a MacBook Air M4 (macOS 27, CrossOver with D3DMetal 3 / GPTK 3, 100 Hz display). It will probably also work in other Wine-based launchers and wrappers that use D3DMetal with DLSS/MetalFX support, but only CrossOver has been tested.

> Not affiliated with Rockstar, NVIDIA, Apple or CodeWeavers. **Story mode only.** Don't use it in Red Dead Online.

## Known issues

- **HUD ghosting.** Generated frames are made from the finished image, HUD included, so HUD elements can smear or double when the camera moves:
  - Minimap glitches → set the radar to *Compass* or *Off*.
  - Crosshair ghosts → when you turn the camera quickly, two faint copies of the crosshair dot appear beside it and seem to orbit around the real one. Set the reticle to *Off* or *Simple*.

  A real fix needs a copy of each frame without the HUD, which RDR2 doesn't make. **If you'd like to fork this and fix it, please do. We recommend it, and the code is MIT-licensed.**
- **DLSS has to be on.** The mod reads its data from DLSS, so if DLSS is off, there's no frame generation. If you don't like DLSS, sorry, but it has to be on. Any mode works; *Ultra Quality* is the highest and looks best.
- CrossOver logs a few missing NVIDIA functions (`SetReflexSync` and others). These are harmless.

## Requirements

- Apple Silicon Mac
- CrossOver with D3DMetal (or, untested, another Wine wrapper with D3DMetal and DLSS/MetalFX support)
- RDR2 in DirectX 12 mode, with **DLSS on** in the game's graphics settings
- In the bottle's CrossOver settings: **DLSS / MetalFX enabled** (`D3DM_ENABLE_METALFX=1`)

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
5. Start the game, turn on DLSS, and play. With the Metal HUD on, you'll see MetalFX frame interpolation listed. VSync on or off both work.

### Why is the mod called `version.dll`?

The game loads a standard Windows file called `version.dll`, and it checks its own folder before the system one. A file with that name next to `RDR2.exe` gets loaded instead, passes everything through to the real one, and runs the mod inside the game. No game files are changed. This is the same trick many mods use (ReShade, OptiScaler); a different name and the game would never load it.

## Turning it off and on

There's no in-game switch: frame generation is on whenever the mod is installed and DLSS is on.

- **Tip:** after installing, select the 7 files in Finder, right-click, and give them a color tag (e.g. red) so you can always find them among the game's files.
- **To turn it off**, move `version.dll` out of the game folder (e.g. into a folder called `framegen-off`). Without it the mod doesn't load and the other 6 files do nothing.
- **To turn it back on**, move `version.dll` back.

Frame generation always makes one extra frame per real frame (2x). The Metal HUD can briefly show a bit more than 2x because of how it averages; that's normal.

## Uninstall

Delete the 7 files and remove the `version` entry from Wine Configuration.

**Be careful with the two similar names:**
- `nvngx_dlssg.dll` (**with** a "g") is frame generation, from this install. Delete this one.
- `nvngx_dlss.dll` (**no** "g") is the game's own DLSS. **Don't delete it**, or DLSS stops working.

## Troubleshooting

The mod writes `ngxlog.txt` in the game folder (new each launch, stays small). Its first line shows the mod version. Look for lines starting with `STREAMLINE`, `SL ERROR` or `EXCEPTION`, and include the file if you report a bug.

Some `SL ERROR` lines are normal under CrossOver and harmless: `setDynamicMFGParams`, `setFlipConfig`, `SetReflexSync`, `NGX Updater not available`, `0xbad00000`, and `ReflexNotDetectedAtRuntime`.

- **No frame generation and no `ngxlog.txt`**: the Wine override from step 4 is missing. It has to be set in Wine Configuration; an environment variable doesn't work.
- **"DLSS-G not supported"** in the log: DLSS/MetalFX isn't enabled in the bottle's settings.
- **Nothing happens**: DLSS is off in the game.

---

## For developers

Players don't need anything below; the download in Releases is ready to use.

### Building

Needs MinGW-w64 (`brew install mingw-w64`) and NVIDIA's Streamline headers:

```bash
git clone --depth 1 --branch v2.14.16 https://github.com/NVIDIA-RTX/Streamline.git streamline/src
```

```bash
./src/build.sh
```

This produces `src/version.dll`.

### How it works

- `src/ngxlog.c` loads as `version.dll` and forwards every call to the real one. It hooks CrossOver's NGX (DLSS) exports to capture depth, motion vectors and jitter on every DLSS evaluate, and hooks DXGI and D3D12 device creation so Streamline can wrap the factory and swapchain (manual hooking, no code patching).
- `src/slfg.cpp` runs Streamline: per-frame tokens, camera constants, resource tags, Reflex markers, and DLSS-G turned on.
- `src/fg.h` is an older path that drives D3DMetal's frame generator directly and presents frames itself. It's kept as a fallback but paces worse.

## License

MIT, see [LICENSE](LICENSE). Fork it, change it, share it. The NVIDIA Streamline files are not included in this repo. They're MIT-licensed by NVIDIA and come from NVIDIA's own release.

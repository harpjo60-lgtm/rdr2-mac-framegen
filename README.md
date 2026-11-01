# RDR2 Mac Frame Gen

Frame generation for **Red Dead Redemption 2** (Windows, DirectX 12) on Apple Silicon Macs running **CrossOver**.

Cyberpunk 2077 gets frame generation on Mac because CrossOver's D3DMetal turns NVIDIA's DLSS Frame Generation into Apple's MetalFX frame interpolation. RDR2 only ships DLSS upscaling, not frame generation. This mod adds the missing piece: it takes the depth and motion data the game already sends to DLSS and feeds it to NVIDIA Streamline's frame generation, which D3DMetal then runs on MetalFX.

Tested on a MacBook Air M4 (macOS 27, CrossOver with D3DMetal 3 / GPTK 3, 100 Hz display). Roughly doubles the frame rate you see.

> Not affiliated with Rockstar, NVIDIA, Apple or CodeWeavers. **Story mode only.** Don't use it in Red Dead Online.

## Requirements

- Apple Silicon Mac, CrossOver with D3DMetal (DirectX 12)
- RDR2 running in DirectX 12 mode with **DLSS turned on** in the game's graphics settings. The mod reads its data from DLSS, so with DLSS off it does nothing.
- In the bottle's CrossOver settings: **DLSS / MetalFX enabled** (`D3DM_ENABLE_METALFX=1`)

## Install

1. Download `version.dll` from this repo's [Releases](../../releases).
2. Download **NVIDIA Streamline SDK v2.14.16** from [NVIDIA's Streamline releases](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16). Get the plain `streamline-sdk-v2.14.16.zip`, **not** the `aarch64` or `arm64ec` ones. The game is an x64 Windows program even on a Mac. From its `bin/x64` folder, take these 6 files:
   - `sl.interposer.dll`
   - `sl.common.dll`
   - `sl.dlss_g.dll`
   - `sl.reflex.dll`
   - `sl.pcl.dll`
   - `nvngx_dlssg.dll`
3. Put all 7 files in the RDR2 game folder, next to `RDR2.exe`. Don't replace or delete the game's own `nvngx_dlss.dll` (no "g").
4. Tell Wine to load the mod's `version.dll`: in CrossOver, open the bottle's **Wine Configuration** → **Libraries**, type `version`, click **Add**, and make sure it says **(native, builtin)**.
5. Start the game, turn on DLSS, and play. With CrossOver's Metal HUD on, you'll see MetalFX frame interpolation listed.

To uninstall, delete the 7 files and remove the `version` entry from Wine Configuration.

## Recommended game settings

The generated frames are made from the finished image, HUD included, so HUD elements that sit still while the world moves can ghost:

- **Minimap glitches** → set the radar to *Compass* or *Off*.
- **Crosshair doubles or triples when you move fast** → set the reticle to *Off* or *Simple*.

VSync on or off both work.

## Known issues

- HUD ghosting (see above). A proper fix needs a copy of each frame without the HUD, which RDR2 doesn't make.
- CrossOver logs a few missing NVIDIA functions (`SetReflexSync` and others). These are harmless.

## Troubleshooting

The mod writes `ngxlog.txt` and Streamline writes `sl.log` in the game folder. Look for lines starting with `STREAMLINE`, `SL ERROR` or `EXCEPTION`.

- **No frame generation and `ngxlog.txt` doesn't exist**: the Wine override from step 4 is missing. It has to be set in Wine Configuration; an environment variable doesn't work.
- **"DLSS-G not supported"** in the log: DLSS/MetalFX isn't enabled in the bottle's settings.
- **Nothing happens**: DLSS is off in the game.

## Building

Needs MinGW-w64 (`brew install mingw-w64`) and NVIDIA's Streamline headers:

```bash
git clone --depth 1 --branch v2.14.16 https://github.com/NVIDIA-RTX/Streamline.git streamline/src
```

```bash
./src/build.sh
```

This produces `src/version.dll`.

## How it works

- `src/ngxlog.c` loads as `version.dll` and forwards every call to the real one. It hooks CrossOver's NGX (DLSS) exports to capture depth, motion vectors and jitter on every DLSS evaluate, and hooks DXGI and D3D12 device creation so Streamline can wrap the factory and swapchain (manual hooking, no code patching).
- `src/slfg.cpp` runs Streamline: per-frame tokens, camera constants, resource tags, Reflex markers, and DLSS-G turned on.
- `src/fg.h` is an older path that drives D3DMetal's frame generator directly and presents frames itself. It's kept as a fallback but paces worse.

## License

MIT, see [LICENSE](LICENSE). The NVIDIA Streamline files are not included in this repo. They're MIT-licensed by NVIDIA and come from NVIDIA's own release.

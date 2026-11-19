# DaedalusX3D (Old 3DS Edition)

🌐 **Languages:** **English** | [Português (Brasil)](README.pt-BR.md)

A fork of **DaedalusX64** focused and tuned specifically for **Nintendo 3DS Old / 2DS** (`ARMv6K` ARM11 MPCore @ 268 MHz, VFPv2, 16 KB L1 I/D-cache, 6 MiB VRAM, PICA200), featuring a pure `picaGL` frontend inspired by PSX3D.

---

## Current Status & Performance (Old 3DS)

- **Mario Kart 64**: currently running between **25 and 30 FPS** on Old 3DS with audio enabled (with occasional minor audio stutters).
- **Frontend**: dual-screen UI in pure `picaGL` (touch list + action bar on the bottom screen, cover carousel/details on the top screen), with live theme switching between **Original (PSX3D)** and **N64 Classic** via `L` / `R`.
- **Implemented Optimizations**:
  - **ARMv6K DynaRec**: direct RDRAM loads/stores for statically known addresses (`LUI + LW/SW/...`), native codegen for `SRL`, 64-bit shifts (`DSLL`/`DSRL`/`DSRA`), `BC1F`/`BC1T`, `CVT.S.W`/`CVT.D.W`/`CVT.W`/`ROUND.W`, and register-leak fix in `ExpireOldIntervals()`.
  - **3D Pipeline (VFPv2)**: hardware vector **VFPv2 `LEN=4`** assembly for vertex transformation and 4×4 matrix multiplication, UV scaling fused into `PrepareTrisUnclipped()`, zero-malloc `TempVerts` scratch buffer, GL state cache, and blend-mode cache.
  - **Textures & VRAM**: direct decoding of `CI4`/`CI8` (`RGBA16`) textures to native `RGBA5551` (50% less VRAM usage and 2× lower swizzle/bandwidth cost).
  - **HLE Audio**: 1-cycle hardware saturation via ARMv6 `SSAT`, branchless inner loop in `ENVMIXER2`, block-based `CAudioBuffer` ring writes, and subtle playback-rate smoothing (`41454–44100 Hz`).

### Roadmap
- [ ] **Graphics backend migration (`picaGL` → `citro3d` / native PICA200)**: planned to eliminate the blocking `LightEvent_Wait` on buffer swaps, enable true CPU/GPU overlap, use hardware TEV stages (reducing multipass overhead), and target locked 30 FPS without stutters.

---

## Releases

Pre-built binaries are published on the GitHub **[Releases](../../releases)** page:
- **`DaedalusX64.3dsx`** — for **Homebrew Launcher**
- **`DaedalusX64.cia`** — for direct Home Menu installation via **FBI**

*(Locally after building, release artifacts are also staged in [`releases/`](releases/README.md).)*

---

## Installation on Nintendo 3DS

### 1. Audio Prerequisite (`dspfirm.cdc`)
The emulator requires the 3DS DSP firmware dump for audio output:
- Ensure `sdmc:/3ds/dspfirm.cdc` exists on your SD card.
- If missing, run **DSP1** (or use Rosalina's DSP firmware dump option) once.

### 2. SD Card Folder Layout
Create the following directory structure on your SD card:

```text
sdmc:/
└── 3ds/
    ├── dspfirm.cdc                  # DSP firmware (required for audio)
    └── DaedalusX64/
        ├── DaedalusX64.3dsx         # Executable (if using Homebrew Launcher)
        ├── Roms/                    # <-- PLACE YOUR ROMs HERE (.z64, .n64, .v64)
        │   ├── Mario Kart 64.z64
        │   └── Super Smash Bros.z64
        ├── SaveGames/               # Native saves (EEPROM / SRAM / Flash / MemPak)
        ├── SaveStates/              # Save states
        └── Resources/               # Optional box art / resources
```

> If you install **`DaedalusX64.cia`** via **FBI**, the ROMs directory remains the same: `sdmc:/3ds/DaedalusX64/Roms/`.

### 3. Controls
- **`SELECT`**: opens the in-game pause menu (Frameskip, Audio, CPU Speed Hack, Viewport, Save/Load State).
- **`L` / `R`** (in the main menu): switches between `Original (PSX3D)` and `N64 Classic` themes.
- **Circle Pad**: N64 analog stick | **D-Pad / Buttons**: per-ROM configurable mapping.

---

## Building from Source (Old 3DS)

### Prerequisites
Install **devkitPro / devkitARM** with `libctru`, `picaGL`, and `cmake`:
- `DEVKITPRO` and `DEVKITARM` environment variables configured.
- ARM `libpng` and `zlib` installed under `build-3ds-local/vendor/` (or pointed to via `DAEDALUS_CTR_DEPS_DIR`).

### Build `.3dsx` and `.cia`
```sh
cmake -S Source -B build-3ds-local \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/Tools/3dstoolchain.cmake" \
  -DCTR_RELEASE=ON

cmake --build build-3ds-local --target N3DS -j"$(nproc)"
```

Generated binaries will be placed in:
- `build-3ds-local/DaedalusX64.3dsx`
- `build-3ds-local/DaedalusX64.cia`
- `build-3ds-local/DaedalusX64.elf`

### Running Host Unit Tests
```sh
sh tests/ctr_host/run.sh
```

---

## Project Structure

- [`Source/Core/`](Source/Core/) — R4300i CPU, memory, DMA, interrupts, RSP HLE, save states.
- [`Source/DynaRec/`](Source/DynaRec/) — Dynamic recompiler infrastructure and fragment cache.
- [`Source/HLEAudio/`](Source/HLEAudio/) — HLE audio mixing (ABIs 1/2/3, ADPCM, resample, ring buffer).
- [`Source/HLEGraphics/`](Source/HLEGraphics/) — Display List parser, T&L, texture cache and conversion.
- [`Source/SysCTR/`](Source/SysCTR/) — Nintendo 3DS platform layer (`DynaRec/arm`, `HLEGraphics`, `HLEAudio`, `UI/FrontendCTR`, `Input`, `Utility`).
- [`Data/`](Data/) — Compatibility database (`roms.ini`), cheats (`Daedalus.cht`), controller configs, and languages.
- [`Tools/`](Tools/) — `devkitARM` CMake toolchain (`3dstoolchain.cmake`).
- [`tests/`](tests/) — Host unit test suite (`ctr_host`).
- [`releases/`](releases/) — Staging directory for release binaries (`.3dsx` / `.cia`).

## License
Based on **DaedalusX64** under the GNU GPL v2 license ([`copying.txt`](copying.txt)).

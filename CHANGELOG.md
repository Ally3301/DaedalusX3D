# Changelog

All notable changes to **DaedalusX3D (Old 3DS Edition)** are documented in this file.

---

## [v0.1.1-Stable] — 2026-11-20

Major frontend overhaul delivering a native Nintendo 3DS / CTR64-inspired visual design, interactive bottom-screen physical-extension controls during gameplay, automatic on-device Wi-Fi box-art scraping, frosted frozen-game pause overlay, and critical ARM DynaRec 64-bit FPU compatibility fixes.

### Frontend & User Experience (`Source/SysCTR/UI/`)
- **Modular 3DS-native Citro3D UI engine** (`UITheme`, `UIAtlas`, `UIRenderer`, `UITouchInput`): replaced the monolithic 2,392-line bitmap-font frontend and removed 14 dead legacy `PSX3D*` files.
- **Single-atlas proportional typography & SDF shapes (`UIAtlas`)**: packs `Roboto-Medium.ttf` in two crisp proportional sizes, 9-slice anti-aliased rounded cards and 2px focus borders, smooth circles/rings, and 22 procedural vector icons into a single `256×256` RGBA8888 texture (1 draw call per screen).
- **Light & Dark 3DS themes (`Y` toggle)**: authentic 3DS matte striped canvas, `#1DB7F2` cyan selection rings, dark charcoal hint bar with circular `(A)/(B)/(X)/(Y)` and pill `[L/R]`/`START` badges, plus *Midnight* and *Stealth* themes.
- **Smooth spring animations & frozen-game Pause Menu**: 2D spring-gliding focus ring across the 3×3 pause grid, ROM list, options, and controls screens; captures a 2× soft-focus snapshot of the live N64 top screen behind an animated frosted pause banner (`Resume`, `Save`, `Load`, `Slot 0–4`, `Audio`, `Video`, `Controls`, `Reset`, `Game list`, `Exit`).
- **Interactive Bottom-Screen Physical-Extension Controls (`TouchControllerCTR`, `Citro3DContext`, `GraphicsContextCTR`, `InputManagerCTR`)**:
  - **4-way Yellow N64 C-Buttons Cross (`C-Cross`, default)** with elastic capture and diagonal slide-across for camera and items.
  - Live 1-tap mode cycling between **C-Cross**, **Virtual C-Stick Camera** (360° 8-way), **Dual-Analog Shooter** (Touch Stick → N64 Main Stick, Circle Pad → C-Buttons), **Virtual N64 D-Pad**, and **Sleep** (pure black battery saver).
  - Dedicated **Z Trigger** + **Z-Lock** toggle, hold-guarded (`0.35s`) **Quick Save / Quick Load**, and compact top HUD strip (live FPS, 1-tap Mute, 3DS Battery, Mode, Sleep, Pause).
  - **Dirty-driven bottom rendering** using a dedicated vertex pool with zero `GX_MemoryFill` / `C3D_FrameSplit` and 0 DMA cost on idle frames.
  - Interactive **Pause → Controls** screen with visual layout schematic, handedness flip, Z-trigger mode, deadzone, sensitivity, and Y-inversion persisted to `touch_config.ini`.
- **Automatic On-Device Box-Art Scraper (`CoverScraperCTR`)**: low-priority background thread using 3DS `httpc` + `ACU_GetWifiStatus` that fetches missing N64 covers from `libretro-thumbnails`, box-downsamples them via `libpng` to compact `≤ 128×128` PNGs under `sdmc:/3ds/DaedalusX64/Resources/Preview/`, prioritizes the highlighted ROM, and refreshes the hero showcase automatically.

### CPU, ARM DynaRec & Audio Compatibility
- **Odd-numbered 64-bit FP registers in ARM DynaRec** (`CodeGeneratorARM.cpp`): fixed `$f7` / odd double-precision register handling across `GenerateSDC1`, `GenerateMOV_D`, `GenerateCMP_D`, and all `*.D` arithmetic/conversion ops (fixes *Conker's Bad Fur Day* 3D display-list corruption and DP FullSync deadlock).
- **Interpreter overlapping `MOV.D` & 64-bit known-value fixes**: fixed `R4300_Cop1_D_MOV` register-pair overlap (`R4300.cpp`), `GenerateOR`/`GenerateORI` 64-bit known-value guards, and lazy sign-extension self-overwrite flushes (`CodeGeneratorARM.cpp`).
- **Audio `ucode_data` bounds check** (`HLEMain.cpp`): masks and validates `pTask->t.ucode_data` against `gRamSize` before reading audio ABI signatures.

### Documentation & Tests
- Added dual-screen screenshots (`docs/screenshots/`) to [README.md](README.md) and [README.pt-BR.md](README.pt-BR.md).
- Expanded host unit test suite (`tests/ctr_host/run.sh`) to 127 checks with automated tests for `UITheme`, `UITouchInput`, `TouchPadEngine`, and `CoverScraper`.

### Release Artifacts
- `DaedalusX64.3dsx` — Homebrew Launcher (`sdmc:/3ds/DaedalusX64/DaedalusX64.3dsx`)
- `DaedalusX64.cia` — Home Menu installation via **FBI**

---

## [v0.1.0-Stable] — 2026-11-20

First stable Old 3DS release featuring the native **Citro3D / PICA200** graphics backend, major **ARMv6K DynaRec** optimizations, **Core 1/2 asynchronous HLE audio offload**, and **selective fragment-cache invalidation** — delivering playable no-frameskip performance with clean audio on **Old Nintendo 3DS / 2DS** in titles such as *Super Smash Bros.* and *Mario Kart 64*.

### Graphics & Rendering (`citro3d` / PICA200)
- **Native `citro3d` / PICA200 backend** (`Citro3DContext`, `daedalus_vshader.v.pica`, `RendererCTR`, `FrontendCTR`): replaced `picaGL` with a single native `citro3d` pipeline owning the GX queue end-to-end, enabling CPU/GPU overlap and eliminating blocking swap waits.
- **Hardware PICA200 TEV combiner** (`PicaTEVCombiner`): compiles N64 1-cycle and 2-cycle color/alpha combiner muxes directly into native 6-stage PICA200 TEV programs, reducing CPU vertex-color multipass overhead.
- **Draw-call batching & fast-state cache**: contiguous `GPU_TRIANGLES` draws (including 2D `TexRect` / `TexRectFlip` / `FillRect`) coalesce into batched submissions, with full-screen `CYCLE_FILL` clears offloaded to hardware `GX_MemoryFill` (`C3D_RenderTargetClear`).
- **Fast inverse square root (`FastInvSqrtCTR`)** in 3D vertex lighting and `TexGen` normal normalization (`BaseRenderer.cpp`).
- **Clean `.cia` Home Menu exit**: fixed shutdown sequence (`FrontendCTR::Shutdown` → `System_Finalize` → `FinaliseCodeBuffer` → `Citro3D::Shutdown`, restoring `APT_SetAppCpuTimeLimit(30)`).

### CPU & ARMv6K DynaRec
- **Selective range invalidation in `CFragmentCache`** (`FragmentCache.cpp`, `Fragment.cpp`, `Dynamo.cpp`, `IndirectExitMap.h`): tracks exact physical RDRAM ranges and incoming direct branch links per fragment so small dynamic code/overlay writes (such as *Super Smash Bros.*'s 4 KiB micro-overlay at `0x8018d0c0`) evict only the ~10 overlapping fragments instead of flushing the entire 6 MiB code buffer (~2,000–3,000 fragments).
- **Eliminated spurious cache flushes**:
  - `gHotTraceCountMap` overflow now clears only the cold-target counter table without wiping compiled fragments (`Dynamo.cpp`).
  - `Patch_osInvalICache_Mario` now checks the 64-byte code-coverage bitmap (`CPU_InvalidateICacheRange`) for data/asset buffers ≥ 16 KiB instead of unconditionally flushing the entire cache (`patch_cache_hle.inl`).
- **4096-entry global direct-mapped indirect jump table (`gIndirectJumpTable`)** in `DynarecStubARM.S` + `FragmentCache.cpp` + `IndirectExitMap.cpp` (~99.9% indirect hit rate in ARM assembly without returning to C++).
- **Inlined delay slots for branch-likely side exits** (`BEQL`/`BNEL`/`BLEZL`/`BGTZL`/`BLTZL`/`BGEZL`/`BC1FL`/`BC1TL` in `Fragment.cpp`): links taken branch-likely side exits directly in native ARM instead of bailing to the C++ interpreter with `EXEC_DELAY` (cut *Mario Kart 64* DynaRec re-entries from ~1,900/frame to ~15–25/frame).
- **Expanded native ARMv6K codegen (`CodeGeneratorARM.cpp`, `N64RegisterCacheARM.cpp`, `DynarecStubARM.S`)**:
  - Lazy 64-bit sign extension (`IsHiSignExtended`) and 7th cached ARM GPR (`ArmReg_R4`).
  - Native `BGEZAL`/`BGEZALL`/`BLTZAL`/`BLTZALL` with constant propagation on `$ra`.
  - Native ARMv6 `_DynaRec_DIV` / `_DynaRec_DIVU` wrappers preserving cached GPRs and VFP registers (zero flush).
  - Direct VFP `VLDR`/`VSTR` on `LWC1`/`SWC1`/`LDC1`/`SDC1` to avoid ARM11 integer↔VFP transfer stalls.
  - Shorter `CMP_S`/`CMP_D`/`BC1` sequences, `DADDIU` sign-extension fix, constant folding (`XORI`/`SUBU`/`SLL`/`SRL`/`SRA`), and no-op D-cache `OP_CACHE`.
- **Fast SP DMA & OSHLE**: aligned `memcpy` fast path in `DMA_SP_CopyFromRDRAM`/`ToRDRAM` (`DMA.cpp`) and inline KSEG0/KSEG1 fast path in `Patch_osVirtualToPhysical` (`patch_tlb_hle.inl`).

### Audio (Core 1 / Core 2 Async HLE Offload)
- **Dedicated async audio RSP worker** (`AudioPluginCTR.cpp`, `HLEMain.cpp`): offloads `Audio_Ucode` mixing/ADPCM/resampling onto **Core 1 (Old 3DS syscore)** / **Core 2 (New 3DS)** with deterministic completion barriers.
- **Exact output-range tracking for ABI1 / ABI2 / ABI3** (`RecordAsyncOutputRanges`): fixed `ABI2 SAVEBUFF2` (`0x15`) count bitfield extraction (`(cmd0 >> 12) & 0xFFF`) and tracked `ABI1 SETBUFF` / `ABI3 SAVEBUFF3` lengths, eliminating false double-buffer stalls on Core 0 in *Mario Kart 64*.

### Developer & Diagnostics
- Optional compile-time `-DCTR_XRAY=ON` profiler (`PerfProfilerCTR.h`, OFF by default in release builds) and optional `sdmc:/3ds/DaedalusX64/autoboot.txt` support for automated testing.
- Verified with `tests/ctr_host/run.sh` (all host unit tests passing).

### Release Artifacts
- `DaedalusX64.3dsx` — Homebrew Launcher (`sdmc:/3ds/DaedalusX64/DaedalusX64.3dsx`)
- `DaedalusX64.cia` — Home Menu installation via **FBI**

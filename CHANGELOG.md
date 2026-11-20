# Changelog

All notable changes to **DaedalusX3D (Old 3DS Edition)** are documented in this file.

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

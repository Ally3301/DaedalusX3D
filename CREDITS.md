# Credits, Acknowledgments & Third-Party Licenses

**DaedalusX3D** is an open-source Nintendo 64 emulator for the Nintendo 3DS family, distributed under the **GNU General Public License v2.0 or later (`GPL-2.0-or-later`)** — see [`copying.txt`](copying.txt) for the full license text.

This project builds upon and references the work of several open-source N64 and 3DS projects. All upstream authors, repositories, and licenses are credited below.

---

## Core Emulator Projects & References

### 1. DaedalusX64 & DaedalusX64-3DS
- **Authors:** StrmnNrmn, Corn, Salvy, kreationz, MasterFeizz, and the DaedalusX64 contributors
- **Repositories:**
  - https://github.com/DaedalusX64/daedalus
  - https://github.com/masterfeizz/DaedalusX64-3DS
- **License:** GNU General Public License v2.0 or later (`GPL-2.0-or-later`)
- **Role:** Primary upstream codebase (R4300i CPU, HLE graphics/audio, OS HLE, memory/DMA/save systems) and the original Nintendo 3DS port.

### 2. ctr64
- **Author:** gl91306
- **Repository:** https://github.com/gl91306/ctr64
- **License:** GNU General Public License v2.0 or later (`GPL-2.0-or-later`)
- **Role & Attribution:**
  - **Frontend UI Design & Visual Assets:** The current dual-screen 3DS frontend visual design, color palette, card styling, 3×3 pause menu layout, and UI visual elements in `Source/SysCTR/UI/` are derived from and modeled after **ctr64** by **gl91306**.
    > **Planned Replacement Notice:** At the request of ctr64's author (`gl91306`), the ctr64-derived UI layout, styling, and visual assets will be replaced with a completely original UI design and original assets in an upcoming release.
  - **ARM DynaRec & 3DS Optimizations:** Reference and inspiration for 3DS ARM dynamic recompiler optimization techniques and 3DS-specific emulation improvements.

### 3. Mupen64Plus & Mupen64Plus-Next
- **Authors:** Richard Goedeken (Richard42), Sven Eckelmann (ecsv), Hacktarux, Zilmar, Bobby Smiles, **Ari64**, the Mupen64Plus team, and the Libretro team
- **Repositories:**
  - https://github.com/mupen64plus/mupen64plus-core
  - https://github.com/libretro/mupen64plus-libretro-nx
- **License:** GNU General Public License v2.0 or later (`GPL-2.0-or-later`)
- **Role & Attribution:** Reference and implementations for ARM dynamic recompiler (**Ari64**'s `new_dynarec`) optimization techniques, register/branch handling, and Nintendo 64 R4300i/RSP emulation behavior.

---

## Libraries, Assets & Toolchain

| Component | Authors / Source | License | Usage |
| :--- | :--- | :--- | :--- |
| **devkitPro / devkitARM / libctru / citro3d** | devkitPro & contributors (https://devkitpro.org/) | zlib License | ARMv6K toolchain, 3DS system services (`ndsp`, `httpc`, `apt`, `hid`), native PICA200 graphics (`citro3d`) |
| **libpng** | Glenn Randers-Pehrson, Guy Eric Schalnat, et al. | libpng License | PNG decoding/encoding for cover art and textures |
| **zlib** | Jean-loup Gailly & Mark Adler | zlib License | Compression/decompression (PNG, ROM/data utilities) |
| **stb_truetype.h** | Sean Barrett (https://github.com/nothings/stb) | MIT / Public Domain | Runtime TrueType glyph rasterization for `UIAtlas` |
| **Roboto-Medium.ttf** | Christian Robertson / Google Fonts | Apache License 2.0 | UI font bundled in RomFS (`Source/SysCTR/Resources/romfs/Roboto-Medium.ttf`) |
| **libretro-thumbnails** | Libretro (https://github.com/libretro-thumbnails/libretro-thumbnails) | Community box-art repository | Optional on-device Wi-Fi box-art lookup (not bundled in binaries) |

---

## Ecosystem & Build Tools

- **[Luma3DS](https://github.com/LumaTeam/Luma3DS)** — Custom firmware providing executable memory SVCs for the ARM DynaRec.
- **[DSP1](https://github.com/zoogie/DSP1)** — Hardware DSP firmware dumper (`dspfirm.cdc`).
- **[Azahar](https://github.com/azahar-emu/azahar)** — Nintendo 3DS emulator used for development and testing.
- **[Project_CTR (`makerom`)](https://github.com/3DSGuy/Project_CTR)** and **[3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool)** — `.cia` packaging tools.

---

## License Compliance Statement

- **DaedalusX3D** as a combined work is distributed under the **GNU General Public License v2.0 or later (`GPL-2.0-or-later`)**, fully compatible with **DaedalusX64**, **ctr64**, and **Mupen64Plus / Mupen64Plus-Next** (all `GPL-2.0-or-later`).
- Full corresponding source code is provided alongside all distributed `.3dsx` and `.cia` binaries in accordance with the GPL.
- This repository does not host or distribute any copyrighted Nintendo 64 ROMs, BIOS files, or commercial game assets.

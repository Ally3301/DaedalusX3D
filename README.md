# DaedalusX3D (Old 3DS Edition)

Fork do **DaedalusX64** focado e otimizado especificamente para **Nintendo 3DS Old / 2DS** (`ARMv6K` ARM11 MPCore @ 268 MHz, VFPv2, 16 KB L1 I/D-cache, 6 MiB VRAM, PICA200), com frontend em `picaGL` puro inspirado no PSX3D.

---

## Status Atual & Desempenho (Old 3DS)

- **Mario Kart 64**: rodando entre **25 e 30 FPS** no Old 3DS com áudio ativo (apresentando apenas alguns *stutterings* pontuais).
- **Frontend**: interface de duas telas em `picaGL` puro (lista touch + barra de ações na tela inferior, carrossel/capas na tela superior), com troca de tema entre **Original (PSX3D)** e **N64 Classic** via `L` / `R`.
- **Otimizações implementadas**:
  - **DynaRec ARMv6K**: acesso direto em RDRAM para endereços constantes (`LUI + LW/SW/...`), codegen nativo para `SRL`, shifts 64-bit (`DSLL`/`DSRL`/`DSRA`), `BC1F`/`BC1T`, `CVT.S.W`/`CVT.D.W`/`CVT.W`/`ROUND.W` e correção de alocação em `ExpireOldIntervals()`.
  - **Pipeline 3D (VFPv2)**: transformação de vértices e multiplicação de matrizes 4×4 em assembly **VFPv2 `LEN=4`**, fusão de escala UV em `PrepareTrisUnclipped()`, zero-malloc em `TempVerts`, cache de estados GL e cache de blend modes.
  - **Texturas & VRAM**: decodificação direta de texturas `CI4`/`CI8` (`RGBA16`) para `RGBA5551` nativo (metade da VRAM e 2× menos banda/swizzle).
  - **Áudio HLE**: saturação em 1 ciclo via instrução `SSAT` ARMv6, `ENVMIXER2` sem branch interno, ring buffer em blocos contíguos e suavização sutil de taxa (`41454–44100 Hz`).

### Roadmap
- [ ] **Migração de backend gráfico (`picaGL` → `citro3d` / PICA200 nativo)**: planejada para eliminar a espera síncrona (`LightEvent_Wait`) no swap de buffers, habilitar *overlap* real entre CPU e GPU, utilizar estágios TEV em hardware (reduzindo multipass) e buscar 30 FPS estáveis sem *stutterings*.

---

## Releases

Os binários prontos para uso ficam disponíveis na aba **[Releases](../../releases)** do GitHub:
- **`DaedalusX64.3dsx`** — para executar pelo **Homebrew Launcher**
- **`DaedalusX64.cia`** — para instalar direto na tela inicial via **FBI**

*(Localmente após compilar, os arquivos também são copiados para [`releases/`](releases/README.md).)*

---

## Instalação no Nintendo 3DS

### 1. Pré-requisito de áudio (`dspfirm.cdc`)
O emulador precisa do dump do firmware DSP do 3DS para reproduzir áudio:
- Verifique se o arquivo `sdmc:/3ds/dspfirm.cdc` existe no cartão SD.
- Se não existir, use o homebrew **DSP1** (ou a opção de dump de DSP do Rosalina) uma única vez.

### 2. Estrutura de pastas no cartão SD
Crie a estrutura abaixo no cartão SD:

```text
sdmc:/
└── 3ds/
    ├── dspfirm.cdc                  # Firmware DSP (obrigatório)
    └── DaedalusX64/
        ├── DaedalusX64.3dsx         # Executável (se usar Homebrew Launcher)
        ├── Roms/                    # <-- COLOQUE SUAS ROMs AQUI (.z64, .n64, .v64)
        │   ├── Mario Kart 64.z64
        │   └── Super Smash Bros.z64
        ├── SaveGames/               # Saves nativos (EEPROM / SRAM / Flash / MemPak)
        ├── SaveStates/              # Save states
        └── Resources/               # Capas/recursos opcionais
```

> Se você instalar o **`DaedalusX64.cia`** via **FBI**, a pasta de ROMs continua sendo a mesma: `sdmc:/3ds/DaedalusX64/Roms/`.

### 3. Controles no Emulador
- **`SELECT`**: abre o menu de pausa durante o jogo (Frameskip, Áudio, CPU Speed Hack, Viewport, Save/Load State).
- **`L` / `R`** (no menu principal): alterna o tema entre `Original (PSX3D)` e `N64 Classic`.
- **C-Pad**: analógico N64 | **D-Pad / Botões**: mapeamento configurável por ROM.

---

## Como Compilar (Build Old 3DS)

### Pré-requisitos
Instale o **devkitPro / devkitARM** com `libctru`, `picaGL` e `cmake`:
- `DEVKITPRO` e `DEVKITARM` configurados no ambiente.
- Dependências ARM (`libpng` e `zlib`) instaladas em `build-3ds-local/vendor/` (ou via `DAEDALUS_CTR_DEPS_DIR`).

### Compilando o `.3dsx` e `.cia`
```sh
cmake -S Source -B build-3ds-local \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/Tools/3dstoolchain.cmake" \
  -DCTR_RELEASE=ON

cmake --build build-3ds-local --target N3DS -j"$(nproc)"
```

Os binários gerados estarão em:
- `build-3ds-local/DaedalusX64.3dsx`
- `build-3ds-local/DaedalusX64.cia`
- `build-3ds-local/DaedalusX64.elf`

### Executando os testes de host
```sh
sh tests/ctr_host/run.sh
```

---

## Estrutura do Projeto

- [`Source/Core/`](Source/Core/) — CPU R4300i, memória, DMA, interrupções, RSP HLE, save states.
- [`Source/DynaRec/`](Source/DynaRec/) — Infraestrutura do recompilador dinâmico e cache de fragmentos.
- [`Source/HLEAudio/`](Source/HLEAudio/) — Mixagem HLE de áudio (ABIs 1/2/3, ADPCM, resample, ring buffer).
- [`Source/HLEGraphics/`](Source/HLEGraphics/) — Parser de Display List, T&L, cache e conversão de texturas.
- [`Source/SysCTR/`](Source/SysCTR/) — Código específico de Nintendo 3DS (`DynaRec/arm`, `HLEGraphics`, `HLEAudio`, `UI/FrontendCTR`, `Input`, `Utility`).
- [`Data/`](Data/) — Banco de compatibilidade (`roms.ini`), cheats (`Daedalus.cht`), configurações de controle e traduções.
- [`Tools/`](Tools/) — Toolchain CMake para `devkitARM` (`3dstoolchain.cmake`).
- [`tests/`](tests/) — Suíte de testes unitários de host (`ctr_host`).
- [`releases/`](releases/) — Pasta preparada para empacotamento de releases (`.3dsx` / `.cia`).

## Licença
Baseado no **DaedalusX64** sob licença GNU GPL v2 ([`copying.txt`](copying.txt)).

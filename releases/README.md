# Releases — DaedalusX3D (Old 3DS)

Esta pasta é destinada aos binários gerados para publicação na aba **Releases** do GitHub (os arquivos `.3dsx` e `.cia` são ignorados pelo `.gitignore` para não inflar o histórico do repositório).

## Arquivos de Release

| Arquivo | Uso | Onde colocar no cartão SD |
| :--- | :--- | :--- |
| `DaedalusX64.3dsx` | Homebrew Launcher (Rosalina / HBL) | `sdmc:/3ds/DaedalusX64/DaedalusX64.3dsx` |
| `DaedalusX64.cia` | Instalação direta na Home Menu (FBI) | Qualquer pasta no SD → instalar via **FBI** |

> **Importante:** Para áudio funcional, certifique-se de que `sdmc:/3ds/dspfirm.cdc` existe no cartão SD (gerado via **DSP1** ou pelo menu Rosalina).
>
> **Pasta das ROMs:** Coloque seus jogos de N64 (`.z64`, `.n64`, `.v64`) em `sdmc:/3ds/DaedalusX64/Roms/`.

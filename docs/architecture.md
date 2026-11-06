# Architecture

## The game

SimCopter (Maxis, 1996) is one PE image, `SimCopter.exe`.

| | |
|---|---|
| Version | 1.0.1.4 (v1.01), FileVersion resource |
| Size | 1,569,280 bytes, SHA-1 `c8da4dc4915724a13ecc5c7ea20774c39c69cc54` |
| Built | PE timestamp `0x33418C93`, 1997-04-01 |
| Toolchain | Microsoft Visual C++, linker 3.10, CRT linked statically; sources under `C:\Copter\Source\` |
| Image | base `0x00400000`, entry `0x004DCF20`, has relocations, no protection, no overlay |

| Section | VA | Virtual size |
|---|---|---|
| `.text` | `0x00401000` | `0xF6600` (about 1 MB of code) |
| `.rdata` | `0x004F8000` | `0x71E0` |
| `.data` | `0x00500000` | `0x12A388` (mostly uninitialised) |
| `.idata` | `0x0062B000` | `0x1A8E` |
| `.rsrc` | `0x0062D000` | `0x4B4E8` (string tables in English, French, German, Italian...) |
| `.reloc` | `0x00679000` | `0x12A16` |

Imports (by DLL): KERNEL32 113, USER32 50, GDI32 21, WINMM 19 (joystick,
mmio), glide.dll 28 (3dfx Glide 2.x), smackw32 17 (by ordinal), VERSION 3,
ADVAPI32 3 (registry), DSOUND 2, comdlg32 2 (Open/Save), DDRAW 1
(`DirectDrawCreate`), MSACM32 1.

Compared with `Streets.exe` (3.4 MB, linker 4.20, 1997-10-22) it is smaller
and older, with a Glide renderer and the software one but no OpenGL.

| File in `game\` | What it is |
|---|---|
| `SimCopter.exe` | The game |
| `smackw32.dll` | RAD Smacker, for the `.smk` videos. Runs natively, not lifted |
| `copter.cfg` | Preferences (a MIFF file the game writes) |
| `bmp\`, `cities\`, `fonts\`, `geo\`, `help\`, `smk\`, `sound\`, `tweak\`, `x\`, `MY SAVED CITIES\` | Data, opened by relative path |

`tools/gamefiles.py` copies the disc's `SIMCOPTER\` folder into `game\SIMCOPTER\`; the host maps `game\` as the CD.

## The pipeline

The same as Streets of SimCity's:

```
disc ── tools\gamefiles.py ── game\SIMCOPTER\SimCopter.exe ── tools\catalog.py ── work\functions.json
                                                     (pcrecomp disasm32)          │
                                                                            run_lift.py
                                                                (pcrecomp generate + lift32)
                                                                                  │
src\runtime\host.c + ddraw.c + present.c + oracle.c + pcrecomp native32 ── build ── src\recomp\gen\recomp_00NN.c
                                                                         │
                                                                   build\sco.exe
```

`sco.exe` is linked at `0x60000000` so the guest's `0x00400000` is free; it
maps `SimCopter.exe` there, binds its imports to Windows (native32) and to
the host's shims, and runs only lifted code.

## What the host changes (inherited from Streets, to be confirmed here)

- **The game folder's parent is the CD.** SimCopter imports `GetLogicalDriveStringsA`,
  `GetDriveTypeA` and `GetDiskFreeSpaceA` and asks "Please insert the
  SimCopter CD-ROM" without a disc. The host maps a free drive letter onto
  `game\` and reports it as a full CD-ROM.
- **DirectDraw is the host's own** (`ddraw.c`): memory surfaces and a
  remembered 640x480x8 mode, presented scaled in a normal window by
  `present.c` (Direct3D 11), so the desktop resolution never changes.
- **Smacker draws into the window's DC**: smackw32's own `GetDC`,
  `StretchDIBits` and friends are pointed at the host's screen.
- **No 3dfx board.** `grGlideInit`, `grSstQueryHardware` and
  `grGlideShutdown` are answered by the host ("no hardware"); the other 25
  Glide imports stay unbound.

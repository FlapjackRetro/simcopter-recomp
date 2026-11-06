# SimCopter — Static Recompilation

Static recompilation of **SimCopter** (Maxis, 1996) from its shipping Win32
binary, `SimCopter.exe`, to native C, on the
[pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain. The goal is the
game's own code, recompiled, running on current Windows: no compatibility
mode, no 640x480 desktop switch, no CD in the drive.

SimCopter's engine is the one [Streets of SimCity](https://github.com/sp00nznet/streetsofsimcity)
was built on, and that recomp already drives its cities on the software
renderer. This project starts from its host and lift driver (see
[NOTICE](NOTICE)).

## Status: bring-up — in game, flying on the software renderer

| Stage | State |
|---|---|
| The build | the disc's `SimCopter.exe` v1.0.1.4 (1.01), 1997-04-01, no protection ([architecture.md](docs/architecture.md)) |
| Project skeleton (host, lift driver, catalog, build) | adapted from Streets of SimCity |
| Rasterizer "return to caller's caller" sites | 42 found by shape ([bringup.md](docs/bringup.md) 2) |
| Function catalog (`tools/catalog.py`) | 6,753 functions, 96.3% of `.text` |
| Lift (`run_lift.py`) | 6,753 functions, 2.2M lines of C, **0 lift errors** |
| Build | builds with MSVC 2022 (x86) |
| CD check | passes: `game\` offered as the CD ([bringup.md](docs/bringup.md) 5) |
| Intro video (Smacker), menus | framed, in a scaled window ([bringup.md](docs/bringup.md) 6-7) |
| New Career Game: into the city, flying | works; camera jitters ([bringup.md](docs/bringup.md) 8) |
| v1.02 (`Upgr1.2\Simcopter102.exe`) | not targeted yet ([bringup.md](docs/bringup.md) 4) |

## Getting started

You need **your own SimCopter CD**, or a copy of its files, or an ISO of it.
Nothing from the game is in this repository and nothing is downloaded for
you. The lifted C is generated on your machine from your copy and is never
distributed.

Prerequisites: Windows 10/11, **Python 3.10+** (`py -3 --version`), **git**,
**Visual Studio 2022** (any edition, or the Build Tools) with *Desktop
development with C++* including the x86 tools, **CMake 3.20+**, **Ninja**,
and the pcrecomp toolkit cloned **beside** this repository:

```
some-folder\
  pcrecomp\      <- git clone https://github.com/sp00nznet/pcrecomp
  simcopter\     <- this repository
```

1. Python packages:
   ```
   py -3 -m pip install --user pefile capstone
   ```
2. Copy the game into `game\SIMCOPTER\`, as it sits on the disc (the
   game's CD check wants `<drive>\SimCopter\`, and the host offers `game\`
   as that drive):
   ```
   py -3 tools\gamefiles.py H:\
   ```
   A copy of the disc's files works too (`py -3 tools\gamefiles.py .` if you
   copied the disc into this folder), or an ISO.
3. The function catalog:
   ```
   py -3 tools\catalog.py
   ```
4. Lift:
   ```
   py -3 run_lift.py
   ```
5. Build (from a plain terminal; `build.cmd` sets up the x86 compiler itself):
   ```
   build.cmd
   ```
6. Dry run (maps and binds the image), then run:
   ```
   build\sco.exe
   build\sco.exe --run --watchdog 60
   ```

## Usage

```
sco [--run] [--headless] [--record out.mp4] [--frames N] [--shot N:file.bmp]
    [--diff A,B] [--key NAME@MS[+HOLD]] [--click X,Y@MS] [--scale N]
    [--fps N] [--res WxH] [-- SimCopter switches]
    [--game game] [--open FILE] [--watchdog S] [--native-trace] [--callbacks]
    [--ddraw-trace] [--original [--lifted VA,...]]
```

The same options as the Streets of SimCity recomp: `--original` runs the
shipping machine code under the same host, for comparison, and
`--original --lifted VA,...` runs just those functions lifted inside it, to
bisect a lift bug.

## What is not in this repo

The game, its data and anything derived from its binary: the function
catalog, the lifted C (`src/recomp/gen/`), seeds and run logs are generated
locally into gitignored folders and never committed. The `.gitignore` is an
allow-list, so a copy of the disc can sit in this folder safely.

## License

The code and documentation here are MIT ([LICENSE](LICENSE)); parts are
adapted from the Streets of SimCity recomp, also MIT ([NOTICE](NOTICE)). The
grant stops at our own work: it does not reach the game, which is
SimCopter © 1996-1997 Maxis, or anything lifted from it.

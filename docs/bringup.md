# Bring-up notes

What has been found, how, and what is still open. Numbered so the README
can point at a section.

## 1. Why pcrecomp, and why Streets of SimCity's host

pcrecomp lifts 32-bit PE images with `disasm32` and `lift32` and runs them on
`native32`. Streets of SimCity (Maxis, 1997) "uses the SimCopter engine" and
its recomp already plays the intro, the menus and drives a city on that
host. SimCopter imports the same things for the same jobs (DirectDraw, the
CD drive calls, Smacker by ordinal, Glide), so its host, lift driver and
catalog were taken as the starting point and renamed (`sco`).

## 2. The rasterizer: returns to the caller's caller

Streets' software rasterizer has edge routines that end `pop eax; ret`,
dropping their own return address; its `run_lift.py` finds them by the
shape `ret; pop r32; ...; ret` inside the rasterizer's address range.

In `SimCopter.exe` that loose shape over all of `.text` gives 53 hits, and
most are not it: compiled epilogues after a `ret`, or `8B C3` (`mov eax,
ebx`) read as a `ret`. Every real one ends a store, `mov [abs32], eax; ret;
pop eax; ret` (`A3 xx xx 50 00 C3 58 C3`), sometimes with `popal` before the
last `ret`. Matching that over all of `.text` gives **42 sites** (Streets
had 44), in two clusters:

- `0x00475B16`-`0x0047BFF9` (18)
- `0x004EF666`-`0x004F64AC` (24)

Two copies of the span code, perhaps for two pixel paths. `run_lift.py`
uses the strict shape and no address range. To confirm once the catalog
exists: that each site's routine is in the catalog and lifts, and that the
second cluster is not CRT code (the CRT starts near the entry, `0x004DCF20`).

## 3. Jump tables in .data

`tools/catalog.py` seeds every run of 8+ `.data` dwords pointing into
`.text`, as Streets does for its unrolled span loops. On `SimCopter.exe`
that is 1,039 addresses (Streets: 1,127).

## 4. Version: 1.01 now, 1.02 later

The disc's game is v1.0.1.4. `Upgr1.2\Simcopter102.exe` is not the game but
a 16-bit InstallShield self-extractor (MZ/NE) that patches an installed
v1.01 to 1.02 (cinematic fixes, throttle and rudder support, an early
Direct3D mode). Getting a 1.02 `SimCopter.exe` means running it once on
Windows (or a VM) against an installed copy. The lift targets 1.01 until
then; addresses in these notes are 1.01's.

### Other builds

A v1.0.1.3 `SimCopter.exe` (1,556,480 bytes, SHA-1
`b5ff1edbf77411d01a474d464b883aa2e278508a`, PE timestamp `0x3314EAFB`,
1997-02-27, linker 3.10, entry `0x004DBB10`) is an earlier Maxis build, five
weeks before the disc's 1.0.1.4: a separate compile (every section moved),
not a byte patch, without the `DirectSoundEnumerateA` import. Not a target;
useful to compare against.

## 5. The CD

First lifted run (6,753 functions, 0 lift errors): the CRT and the game's
start-up ran, then "Please insert the SimCopter CD-ROM", though the host had
mapped `Z:` onto `game\` and reported it as a CD-ROM.

`sub_00435F20` walks `GetLogicalDriveStringsA` (called through `edi`, so no
`call [iat]` names it), lower-cases each root, and takes a drive whose
`GetDriveTypeA` is `DRIVE_CDROM` (or, oddly, `DRIVE_REMOVABLE` from `c:` up).
It appends `SimCopter\` (the string at `0x00500FC8`) and asks `sub_00436260`
whether that is the disc. That routine first wants the drive full:
`sub_00464BE0` returns the drive's free bytes from `GetDiskFreeSpaceA`, and
anything but 0 fails. Then it looks for its files under `<drive>\SimCopter\`.

So the CD is not the game folder but its parent, as on the disc (`H:\SIMCOPTER\`).
`tools/gamefiles.py` now copies the game to `game\SIMCOPTER\`, the host runs
it from there and maps `game\` as the CD.

## 6. The intro: framed off the picture

With the CD found: DirectDraw (640x480x8, a 1334x1000 back surface), the
window, and the intro played, but drawn down and to the right, the Maxis logo
cut off at the bottom right.

The game plays its `.smk` files with `SmackBufferOpen` and
`SmackBufferBlit` onto `GetDC` of its window, which the host already turns
into a DC over its 640x480 screen (Streets' fix). The position was the
problem: `sub_0044A030` takes `GetClientRect` of the window and centres the
movie in it. The presenter sizes the real window to the player's screen, so
the client area was about 1650x1250 and the movie was centred on that, in
640x480 coordinates. The host now answers `GetClientRect` of the game's window
with 640x480. (`GetWindowRect`/`ClientToScreen` at `0x00421120` and
`GetSystemMetrics` at `0x004213A7` may want the same once the mouse is in use.)

## 7. The window: the game's, or the presenter's

With `GetClientRect` answered 640x480, the intro was framed, but the window
shrank to a plain 640x480 one. The game sizes its own window for a 640x480
desktop: `sub_004212D0` (fullscreen) makes it a `WS_POPUP` and
`SetWindowPos`'s it to 640x480; `sub_004211D0`/`sub_004213A0` (windowed) give
it a caption and `MoveWindow` it to the middle of `GetSystemMetrics`' screen.
Before, it had read the real (presenter-sized) client area and "resized" the
window to about what it was, and sized a back surface to match (1334x1000
instead of 640x480); that surface is likely what the green transitions were.

The presenter owns the real window, so for the game's window the host now
accepts `SetWindowPos`, `MoveWindow` and `SetWindowLongA` style changes
without doing them, answers `GetWindowRect` with 0,0-640,480 and leaves
`ClientToScreen` points as they are, and `GetSystemMetrics` reports a
640x480 screen: the 1997 desktop the game expects.

With that, the intro was framed and the window large, but the menu was black:
Streets' DirectDraw took every blit to the primary relative to the real
window's screen position (`to_primary`, from `ClientToScreen` of the real
window). The game's own `ClientToScreen` now answers 0,0, so its coordinates
are already client-relative and the subtraction put the menu off the
picture. `to_primary` no longer moves them. (Before these shims, the game's
real-window coordinates and the host's subtraction had cancelled out.)

## 8. Open questions for the first runs

- Whether the Glide probe is `grGlideInit` + `grSstQueryHardware` as the
  imports suggest, and what the game does when it answers "none".
- Guarded divides: Streets divides inside `try { } catch (...)` and relies on
  the CPU's fault. A lifted `idiv` raises nothing. Look for the same here.
- The registry (`RegOpenKeyA`, `RegQueryValueExA`): what an installed copy
  is expected to have written.
- The 2D cockpit and map: drawn with GDI or into DirectDraw surfaces.

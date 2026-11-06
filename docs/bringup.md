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

## 8. In game

With the menu back, New Career Game loads the city and the helicopter flies on
the software renderer, with no faults. Seen: the camera jitters now and then.
Open: whether the shipping code does the same under this host (`--original`),
which says a game behaviour (the main loop never sleeps, so it runs at many
hundreds of frames a second) or a lift bug.

## 9. Speed, jitter and the black lawn: the game's own

`--original` first stopped at once: oracle.c expected Streets' 35 unbound
Glide imports (`glide2x.dll`); SimCopter has 25 (`glide.dll`). With that
fixed, the shipping code runs under the host too, and shows the same camera
jitter and the same black lawn as the lift (not the lift's white speckles on
it, still open). So those are the game on this machine, not the lift.

It also overspeeds. The main loop (`sub_004308B0`) runs a frame per pass,
the clock is `timeGetTime`, and a pass takes well under a millisecond here:
everything advances per pass, and the camera moves on a clock that steps 1-16
ms at a time. The host now paces the loop: the `PeekMessageA` that finds the
queue empty, the end of a pass, waits for the next tick of `--fps` (default
30, `--fps 0` as shipped), on the performance counter, with
`timeBeginPeriod(1)`. Messages are not delayed.

The black lawn: in flight 10-15 and 224-233 of the game's palette are black
(`flags 05`), and only the static colours come from the system palette, so it
is not the stale-palette idea. F12 (`palette-N.txt`) is the tool for it.

## 10. Higher resolution: `--res WxH`

The game's own windowed mode (`-w`, Readme.txt) renders the 3D view at the
size of its window (before the window shims it made a 1334x1000 back surface
from the real one). `--res 1600x1200` uses that: the game gets `-w`, its
window and desktop answer as that size (`GetClientRect`, `GetWindowRect`,
`GetSystemMetrics`), its desktop as 8-bit (`GetDeviceCaps` BITSPIXEL/PLANES,
checked in `sub_00402CF0`: windowed wants 256 colours), and the host's
screen, shadow and presenter are that size (`ddh_vw`/`ddh_vh` replace the
640x480 constants). The presenter scales it to the real window as before,
aspect locked.

Windowed, the game may colour the screen through a realized GDI palette
rather than one on the DirectDraw primary, and animate it with
`AnimatePalette` (`sub_0045C5A0`): with no palette on the primary the screen
now shows the realized one, and follows `AnimatePalette`.

Expected rough edges: videos centred at their own size (Smacker draws 1:1),
and menus laid out for 640x480.

### The game's own screen modes

Windowed (`-w`) turned out to be fixed at 640x480: the game pins its window
size (`WM_GETMINMAXINFO` in `sub_00421450`), and the flight view stayed
640x480 in a 1280x960 screen however the window was moved.

The real lever is a global, `0x005017D0`, the screen mode.
`sub_004641C0` maps it to the screen size: 0 = 320x240, 1 = 640x480,
2 = 800x600, 3 = 1024x768. `sub_00413170` sizes the 3D view from it (screen
minus a border per mode: 40, 80, 100, 128), and the window setup at
`0x00430F9E` has a rectangle for each. Only one write sets it: `= 1` at
`0x0045DE77`, during start-up. The cockpit layouts (`sub_004124C0`,
`sub_00412860`) have a branch for mode 1 only, so 2 and 3 look like an
unfinished feature switched off.

`--res 800x600` / `--res 1024x768` now set it (run_lift.py `SCREEN_MODE`, a
line after `0x0045DE77`; `sco_screen_mode` in host.c) and the game runs
fullscreen at that size: the display mode, the host's screen and the window
answers follow. Beyond 1024x768 would mean extending the table.

First try at 1024x768: the screen was 1024x768 but the 3D view 640x480 in
its corner, and no cockpit. The same start-up function writes two screen
descriptions into the app object (`+0x4040` and `+0x404C`: 640, 480, 8),
which the renderer sizes from; the patch now sets those to the `--res` size
too. The cockpit layouts only know mode 1, so they are next.

## 11. Render scale: `--res 1280x960` / `1920x1440`

The game's larger modes have no cockpit layout: `sub_004124C0` fills its
table of eight panel rectangles only in mode 1, and the panels are fixed-size
bitmaps inside a widget tree (`sub_00404300`), with gauges and buttons at
offsets inside them. So the 2D stays as designed, at 640x480, and only the 3D
is rendered larger.

A Ctrl+F12 trace (3 frames of DirectDraw calls) shows the frame: a colour
fill of (0,0)-(560,400) on a 640x480 offscreen frame buffer, a `Lock` of it
(the 3D renderer writes there), ten cockpit `Blt`s at 1:1 in 640x480
coordinates, and the frame buffer `Blt` to the primary.

With `--res 640k x 480k` (k = 2, 3, 4) the game stays in mode 1 and:

- the primary and the frame buffer (the first 640x480 offscreen surface after
  the primary) are 640k x 480k pixels, addressed by the game in 640x480: every
  `Blt`/`BltFast`/fill onto or from them is scaled by k and stretched
  (nearest neighbour, colour key kept);
- `GetDC` on them, and the window DC Smacker draws into, carry a world
  transform of k, so GDI text and the videos scale too;
- the 3D view is k times larger: `sub_00413170` takes the screen size from
  `sub_004641C0` and makes the view either all of it (no cockpit, on foot,
  `[esi+0x30] == 0`) or, in mode 1, the screen less an 80-pixel border; the
  screen size is scaled as it returns (run_lift.py `VIEW_SCREEN`, after
  `0x00413180`) and the border with it (`VIEW_BORDER`, after `0x004131AF`).
  The renderer, given the full-size frame buffer by `Lock`, draws it at full
  resolution. (A first version scaled only the bordered view: in flight was
  right, on foot stayed 640x480 in the corner.)
- the window, the mouse and `WM_SIZE` stay in 640x480 (`ddh_gw`/`ddh_gh`).

### 2D drawn straight into the frame buffer

The catalog's highlight box (and its paperclip and coffee stain) came out at
half position and half size: a Ctrl+F12 trace there shows no Blts at all,
only four `Lock`s of the frame buffer a frame: 2D code writing pixels in
640x480 coordinates into what is now a 640k-wide buffer. The 3D renderer
locks the same buffer, but only through the frame setup that also runs the
Low/High resolution switch (`call [eax]` at `0x0046449B`, or `0x004643FF` in
low res), so its return address is on the guest stack. A `Lock` of a k-sized
surface without it gets a 640x480 copy (top-left pixel of each k x k block);
`Unlock` writes back the pixels that changed, as k x k blocks. The copy keeps
the real pitch: the game's surface wrapper remembers the pitch of the last
`Lock` and the on-foot 3D path reuses it (a pitch of 640 squeezed that view
into the top half).

The return address alone missed the on-foot view: there the frame function
locks directly, and the stack shows only exception-handler stubs
(`0x0044C368`, `0x00430A62`). What every 3D frame does have is a colour
fill of its view right before the `Lock`; the catalog's locks have none. So
a `Lock` straight after a fill of the (k-sized) view is 3D too. Those fills
reach past 640x480, so they are already in pixels and are not scaled again
(they had been: `(0,0)-(2560,1920)`).

Open: the pilot's size against the world changes with k (to compare at
640x480, 1280x960, 1920x1440 from the same spot).

## 12. Open questions

- Whether the Glide probe is `grGlideInit` + `grSstQueryHardware` as the
  imports suggest, and what the game does when it answers "none".
- Guarded divides: Streets divides inside `try { } catch (...)` and relies on
  the CPU's fault. A lifted `idiv` raises nothing. Look for the same here.
- The registry (`RegOpenKeyA`, `RegQueryValueExA`): what an installed copy
  is expected to have written.
- The 2D cockpit and map: drawn with GDI or into DirectDraw surfaces.

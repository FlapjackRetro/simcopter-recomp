#!/usr/bin/env python3
r"""Put your copy of SimCopter's files in game/, from the disc.

    py -3 tools/gamefiles.py H:\                    # the CD in a drive
    py -3 tools/gamefiles.py C:\path\to\disc-copy   # a copy of the disc's files
    py -3 tools/gamefiles.py SimCopter.iso          # a 2048-byte ISO

The disc is plain files, no cabinets. The game is the SIMCOPTER\ folder:
SimCopter.exe, smackw32.dll, copter.cfg and the data folders it opens by
relative path from its own folder (bmp, cities, fonts, geo, help, smk, sound,
tweak, x, MY SAVED CITIES). It is copied to game\SIMCOPTER\, as it sits on
the disc: the game's CD check looks for <drive>\SimCopter\ on a CD-ROM, so
the host offers game\ as that drive (docs/bringup.md 5). Setup, DirectX, the demos, the
Classics menus and the 1.02 updater are not copied.

The disc carries both long names and their 8.3 aliases (mysave~1 beside
MY SAVED CITIES); only the long names are copied.

Disc images are read with pcrecomp's ISO9660 walker (tools/assets/iso_peek.py,
Joliet tree), so nothing is mounted. This supplies your own copy to the local
build; nothing is downloaded, and none of it is ever committed.
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'game', 'SIMCOPTER')
GAME_DIR = 'SIMCOPTER'
ALIAS = re.compile(r'~\d')          # an 8.3 alias of a long name: scroll~1.bmp


def target(p):
    """Where disc path p goes under game/, or None."""
    top, _, rest = p.partition('/')
    if top.upper() != GAME_DIR or not rest:
        return None
    if any(ALIAS.search(part) for part in rest.split('/')):
        return None
    return rest


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    if os.path.isdir(src):
        files = [(os.path.relpath(os.path.join(d, f), src).replace('\\', '/'), os.path.join(d, f))
                 for d, _, fs in os.walk(src) for f in fs]
        reader = None
    else:
        sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'pcrecomp')),
                                        'tools', 'assets'))
        import iso_peek
        reader = iso_peek.Reader(src)
        files = [(p, (off, n)) for p, off, n in iso_peek.walk(reader)]
    n = total = 0
    for p, where in files:
        rel = target(p)
        if rel is None:
            continue
        dst = os.path.join(OUT, *rel.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if reader is None:
            shutil.copyfile(where, dst)
            total += os.path.getsize(where)
        else:
            off, size = where
            with open(dst, 'wb') as f:
                f.write(reader.read(off, size))
            total += size
        n += 1
    if not os.path.exists(os.path.join(OUT, 'SimCopter.exe')):
        sys.exit('no SIMCOPTER\\SimCopter.exe in %s: is this the SimCopter disc?' % src)
    print('%d files, %.1f MB in %s' % (n, total / 1e6, OUT))


if __name__ == '__main__':
    main()

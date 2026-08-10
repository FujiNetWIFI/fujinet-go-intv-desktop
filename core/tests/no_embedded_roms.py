#!/usr/bin/env python3
"""Assert that a WITH_INTV_ROMS=OFF binary really carries no Mattel ROMs.

This is the test that makes the COMPLIANCE.md claim checkable rather than
aspirational: it takes a distinctive slice out of each ROM image in
tools/jzintv/roms and greps the built binary for it. Anything found is a ROM
that got compiled in despite the option, which would make the artifact
non-redistributable.

Unlike the CoCo port, there is no freely-redistributable class of ROM here:
EXEC and GROM are both Mattel firmware, so a WITH_INTV_ROMS=OFF binary is
expected to contain NEITHER of them.

usage: no_embedded_roms.py <binary> <rom-dir>
"""

import sys
from pathlib import Path

PROBE = 64


def probe_bytes(data):
    """A slice distinctive enough that finding it means something.

    Reject any chunk dominated by a single byte value and require real
    variety, so a run of 0x00 or 0xFF padding -- which occurs in almost any
    binary of a reasonable size -- can never be mistaken for a match. Scan the
    whole file rather than a few fixed offsets.
    """
    max_run = PROBE // 2      # no single byte value may cover half the slice
    min_distinct = 12

    for start in range(0, max(1, len(data) - PROBE), PROBE):
        chunk = data[start:start + PROBE]
        if len(chunk) != PROBE:
            continue
        if max(chunk.count(b) for b in set(chunk)) > max_run:
            continue
        if len(set(chunk)) < min_distinct:
            continue
        return chunk
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    romdir = Path(sys.argv[2])

    checked = 0
    leaked = []
    unprobeable = []

    for f in sorted(romdir.iterdir()):
        if not f.is_file() or f.suffix.lower() not in (".rom", ".bin"):
            continue
        data = f.read_bytes()
        if len(data) < 128:
            continue
        chunk = probe_bytes(data)
        if chunk is None:
            # Nothing distinctive enough to search for -- report it rather
            # than quietly treating it as checked.
            unprobeable.append(f.name)
            continue
        checked += 1
        if chunk in binary:
            leaked.append(f.name)

    if checked == 0:
        sys.exit("no_embedded_roms: no ROM images found in "
                 f"{romdir} -- the test would pass vacuously")

    for name in leaked:
        print(f"FAIL: {name} is embedded in a WITH_INTV_ROMS=OFF build")

    if leaked:
        return 1
    if unprobeable:
        print("no_embedded_roms: NOT checked (no distinctive slice): "
              + ", ".join(sorted(unprobeable)))
    print(f"no_embedded_roms: {checked} ROM images checked; none embedded, "
          "as intended")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Turn a freshly-copied jzIntv src/ tree into the one core/CMakeLists.txt
compiles: LF-normalized, with the FujiNet mailbox peripheral patched in.

Usage: patch-staged-tree.py <staged-dir> <fujinet-patch>

<staged-dir> is core/jzintv-generated (cmake/StageJzIntv.cmake has already
copied <jzintv-src>/src into <staged-dir>/src). <fujinet-patch> is
tools/jzintv/jzintv-fujinet.patch.

Two steps, in this order because the patch was generated against an
LF-normalized tree (see jzintv-fujinet.patch's own header for why -- jzIntv's
upstream zip mixes CRLF with a handful of stray LF lines, which is enough to
make `patch` refuse hunks that apply cleanly by content):

  1. Normalize every text source file under <staged-dir>/src to LF.
  2. Apply <fujinet-patch> with `patch -p1`.

No other source edits are needed. jzIntv selects its graphics/sound/event/
platform backend entirely by *which files* are compiled (gfx_sdl2.c vs
gfx_null.c, never #ifdef'd inside a shared file), and core/CMakeLists.txt
simply never lists the SDL-backed ones -- core/jzintv/desktop/*_desktop.c take
their place instead. Nothing in the staged tree needs to be told about that
substitution; the object file list *is* the substitution.
"""

import subprocess
import sys
from pathlib import Path

# Extensions jzIntv actually ships as CRLF/mixed text. Binary ROM images and
# the pre-generated bincfg lexer/parser tables are left untouched -- see
# below.
TEXT_EXTS = {
    ".c", ".h", ".py", ".bas", ".txt", ".md",
}
TEXT_BASENAMES = {
    "Makefile", "Makefile.common",
}


def is_text_source(path: Path) -> bool:
    if path.suffix in TEXT_EXTS:
        return True
    if path.name in TEXT_BASENAMES or path.name.startswith("Makefile."):
        return True
    if path.name == "subMakefile":
        return True
    return False


def normalize_line_endings(staged_src: Path) -> int:
    n = 0
    for path in staged_src.rglob("*"):
        if not path.is_file() or not is_text_source(path):
            continue
        data = path.read_bytes()
        if b"\r" not in data:
            continue
        path.write_bytes(data.replace(b"\r\n", b"\n").replace(b"\r", b"\n"))
        n += 1
    return n


def apply_fujinet_patch(staged_dir: Path, patch_path: Path) -> None:
    result = subprocess.run(
        ["patch", "-p1", "--forward", "-i", str(patch_path)],
        cwd=staged_dir,
        capture_output=True,
        text=True,
    )
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main() -> int:
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    staged_dir = Path(sys.argv[1])
    patch_path = Path(sys.argv[2])

    staged_src = staged_dir / "src"
    if not staged_src.is_dir():
        sys.exit(f"patch-staged-tree.py: {staged_src} is missing")

    n = normalize_line_endings(staged_src)
    print(f"patch-staged-tree.py: normalized {n} file(s) to LF")

    apply_fujinet_patch(staged_dir, patch_path)
    print("patch-staged-tree.py: FujiNet patch applied")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

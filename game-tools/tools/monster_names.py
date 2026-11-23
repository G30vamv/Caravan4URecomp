#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
"""Write the monster names of the user's own game to <game folder>/monster_names.txt (read by the Windows shell to
label the life bar). Nothing of the game is shipped: the names are read, on the user's machine, from the game's own
message files (msg/emName_<lang> inside <lang>/data/core_common.arc of the RomFS).

    python3 tools/monster_names.py GAME_FOLDER [--out FILE]

GAME_FOLDER is the prepared game folder (it has source.txt with base= and update= paths, or romfs.bin). The output has one
line a name: `<lang><TAB><id><TAB><name>`, lang being eng, fre, ger, ita or spa; id is the row of the game's monster
table (the em number: 1 Rathian, 2 Rathalos...).
"""
from __future__ import annotations

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import romfs_image  # noqa: E402

LANGS = ("eng", "fre", "ger", "ita", "spa")


def arc_entries(data: bytes):
    """The entries of a MT Framework ARC (3DS): name[0x40], type hash, packed size, size|flags, offset, from byte 12."""
    if data[:4] != b"ARC\0":
        raise ValueError("not an ARC")
    (count,) = struct.unpack_from("<H", data, 6)
    for i in range(count):
        o = 12 + i * 0x50
        name = data[o:o + 0x40].split(b"\0")[0].decode("latin1")
        _ext, packed, size, offset = struct.unpack_from("<IIII", data, o + 0x40)
        yield name, packed, size & 0x1FFFFFFF, offset


def read_msg(arc: bytes, wanted: str) -> bytes | None:
    for name, packed, size, offset in arc_entries(arc):
        if name.lower().replace("/", "\\") == wanted.lower():
            raw = arc[offset:offset + packed]
            return raw if packed == size else zlib.decompress(raw)
    return None


def parse_lmd(data: bytes) -> list[str]:
    """The strings of an 'lmd' message file, by id (a list of 12-byte records, offset/length in UTF-16 units)."""
    if data[:4] != b"lmd\0":
        raise ValueError("not an lmd file")
    count = struct.unpack_from("<I", data, 8)[0]
    # the records sit just before the first string: find the table by walking back from the string data
    first_text = None
    table = None
    # header words: [8] count, [0x14] hash count-ish; the 12-byte records start 12 bytes before the first record with a
    # valid offset, so scan for the longest run of valid records
    best = (0, None)
    for start in range(0x20, min(len(data) - 12, 0x4000), 4):
        n = 0
        o = start
        while n < count + 2 and o + 12 <= len(data):
            off, a, b = struct.unpack_from("<III", data, o)
            if a != b or off < start or off + a * 2 > len(data):
                if not (n == 0 and off == 0 and a == 0 and b == 0):
                    break
            n += 1
            o += 12
        if n > best[0]:
            best = (n, start)
    n, table = best
    if table is None or n < 8:
        raise ValueError("no string table found")
    out = []
    for i in range(n):
        off, a, _b = struct.unpack_from("<III", data, table + 12 * i)
        out.append(data[off:off + a * 2].decode("utf-16-le", "replace") if a else "")
    return out


def main(argv: list[str]) -> int:
    if not argv or argv[0].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    game = argv[0]
    out_path = os.path.join(game, "monster_names.txt")
    if "--out" in argv:
        out_path = argv[argv.index("--out") + 1]
    images = []
    for key in ("update=", "base="):
        src = os.path.join(game, "source.txt")
        if os.path.exists(src):
            for line in open(src, encoding="utf-8"):
                line = line.strip()
                if line.startswith(key):
                    p = line[len(key):]
                    images.append(p if os.path.isabs(p) else os.path.join(game, p))
    for name in ("romfs_update.bin", "romfs.bin"):
        if os.path.exists(os.path.join(game, name)):
            images.append(os.path.join(game, name))
    if not images:
        print("no game source found in", game, file=sys.stderr)
        return 1
    lines = []
    for lang in LANGS:
        arc = romfs_image.read_file(images, f"/{lang}/data/core_common.arc")
        if arc is None:
            continue
        msg = read_msg(arc, f"{lang}\\msg\\emName_{lang}")
        if msg is None:
            continue
        strings = parse_lmd(msg)
        # the table starts with an empty record, then id 0 ("No enemy"), id 1...
        names = strings[1:] if strings and strings[0] == "" else strings
        for i, s in enumerate(names):
            if s and "\t" not in s and "\n" not in s:
                lines.append(f"{lang}\t{i}\t{s}")
    if not lines:
        print("no monster names found", file=sys.stderr)
        return 1
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, out_path)
    print(f"{out_path}: {len(lines)} names")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

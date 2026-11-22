# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
# khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES in LICENSES.
"""Downloads only the part of llvm-mingw that building the game's code needs.

The whole llvm-mingw release is a zip of about 190 MB (600 MB unpacked) with four targets, two C++ libraries and a hundred
tools. Compiling and linking the game's C code for this PC needs 50 files (about 135 MB unpacked): the driver, clang, lld,
the two shared libraries clang is made of, the headers the code includes and the few libraries it links. The zip is read
with HTTP range requests, so the members that are not needed are never downloaded.

Nothing is trusted from the server beyond its bytes: every file must match the SHA-256 in toolchain_manifest.txt (made from
the release that build_game.py pins by the SHA-256 of its whole zip), or nothing is installed. If anything goes wrong
(no range requests, no Internet, a file that differs) this exits with a code that is not 0 and leaves nothing behind;
PrepareGame.ps1 then lets build_game.py download the whole compiler, as before.

    python fetch_toolchain.py --dest <game folder>\\toolchain
"""
import argparse
import hashlib
import io
import os
import shutil
import sys
import urllib.request
import zipfile

NAME = "llvm-mingw-20241217-ucrt-x86_64"
URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20241217/{NAME}.zip"
MARKER = "bin/x86_64-w64-mingw32-clang.exe"
BLOCK = 4 << 20          # the least a request asks for
GAP = 256 << 10          # members this close are fetched in one request
RUN_MAX = 24 << 20       # the most one request asks for to be kept in memory


def load_manifest(path):
    files = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            digest, size, name = line.split(" ", 2)
            files[name] = (digest.lower(), int(size))
    return files


class RangeFile(io.RawIOBase):
    """A remote file as a seekable, read-only file, read with range requests."""

    def __init__(self, url):
        self.pos = 0
        self.fetched = 0
        self.start = 0
        self.data = b""
        # the first request finds where the file really is (GitHub redirects to a signed address) and how big it is
        request = urllib.request.Request(url, headers={"Range": "bytes=0-0", "User-Agent": "Caravan4URecomp"})
        with urllib.request.urlopen(request, timeout=30) as r:
            if r.status != 206:
                raise OSError("the server does not answer range requests")
            self.url = r.geturl()
            self.size = int(r.headers["Content-Range"].rsplit("/", 1)[1])

    def _fetch(self, start, end):
        end = min(end, self.size)
        last = None
        for _ in range(3):
            try:
                request = urllib.request.Request(self.url, headers={"Range": f"bytes={start}-{end - 1}", "User-Agent": "Caravan4URecomp"})
                with urllib.request.urlopen(request, timeout=60) as r:
                    if r.status != 206:
                        raise OSError("a range request was not answered as one")
                    data = r.read()
                if len(data) != end - start:
                    raise OSError("a range came back short")
                self.fetched += len(data)
                self.start, self.data = start, data
                return
            except OSError as e:
                last = e
        raise last

    def prefetch(self, start, end):
        if not (self.start <= start and end <= self.start + len(self.data)):
            self._fetch(start, end)

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        self.pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset
        return self.pos

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        if n == 0:
            return b""
        if not (self.start <= self.pos and self.pos + n <= self.start + len(self.data)):
            self._fetch(self.pos, self.pos + max(n, BLOCK))
        off = self.pos - self.start
        out = self.data[off:off + n]
        self.pos += n
        return out

    def readinto(self, b):
        data = self.read(len(b))
        b[:len(data)] = data
        return len(data)


def installed(final, manifest):
    """True when the compiler is already there as the manifest says (names and sizes)."""
    for name, (_, size) in manifest.items():
        path = os.path.join(final, *name.split("/"))
        if not os.path.isfile(path) or os.path.getsize(path) != size:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", required=True, help="the folder the compiler goes into (its llvm-mingw folder is made inside)")
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "toolchain_manifest.txt"))
    args = ap.parse_args()
    manifest = load_manifest(args.manifest)
    final = os.path.join(args.dest, NAME)
    if installed(final, manifest):
        print("the compiler is already here")
        return 0
    print(f"downloading {URL} (only the {len(manifest)} files that building needs)", flush=True)
    rf = RangeFile(URL)
    zf = zipfile.ZipFile(rf)
    members = {}
    for info in zf.infolist():
        if "/" in info.filename and not info.filename.endswith("/"):
            rel = info.filename.split("/", 1)[1]
            if rel in manifest:
                members[rel] = info
    missing = sorted(set(manifest) - set(members))
    if missing:
        raise SystemExit(f"the release does not have {missing[:3]}...: it is not the one this list was made from")
    picked = sorted(members.items(), key=lambda kv: kv[1].header_offset)
    total = sum(i.compress_size for _, i in picked) or 1
    stage = os.path.join(args.dest, NAME + ".part")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    done = 0
    shown = -1
    try:
        index = 0
        while index < len(picked):
            first = picked[index][1]
            run_start = first.header_offset
            run_end = first.header_offset + 4096 + first.compress_size
            j = index + 1
            while j < len(picked):
                nxt = picked[j][1]
                nxt_end = nxt.header_offset + 4096 + nxt.compress_size
                if nxt.header_offset - run_end > GAP or nxt_end - run_start > RUN_MAX:
                    break
                run_end = max(run_end, nxt_end)
                j += 1
            rf.prefetch(run_start, run_end)
            for rel, info in picked[index:j]:
                target = os.path.join(stage, *rel.split("/"))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                digest = hashlib.sha256()
                size = 0
                with zf.open(info) as src, open(target, "wb") as out:
                    while True:
                        chunk = src.read(1 << 20)
                        if not chunk:
                            break
                        out.write(chunk)
                        digest.update(chunk)
                        size += len(chunk)
                want_digest, want_size = manifest[rel]
                if digest.hexdigest() != want_digest or size != want_size:
                    raise SystemExit(f"{rel}: it is not the file the list says (a changed or damaged download): nothing was installed")
                done += info.compress_size
                if done * 100 // total != shown:  # a line for each percent: the program reads them
                    shown = done * 100 // total
                    print(f"  {done >> 20}/{total >> 20} MB", flush=True)
            index = j
        if not os.path.isfile(os.path.join(stage, *MARKER.split("/"))):
            raise SystemExit("the compiler is not in what was downloaded")
        shutil.rmtree(final, ignore_errors=True)
        os.replace(stage, final)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    print(f"the compiler is installed ({rf.fetched >> 20} MB downloaded)", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception as e:  # no Internet, no range requests...: the caller falls back to the whole download
        print(f"the small download did not work: {e}", flush=True)
        sys.exit(3)

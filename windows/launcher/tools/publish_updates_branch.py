# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Adapted from publish_release.py of the author's other launcher project: the files the updater downloads live in the `updates`
# branch (commit-pinned raw URLs), so the release page only shows the zip, its SHA-256 and manifest.json.
"""Puts the files of a package in the `updates` branch of the repo and rewrites the URLs of manifest.json to point to them.

    python publish_updates_branch.py --version 0.4.0 --package-dir <package folder> --manifest <manifest.json of make_manifest.py>
           --out <folder for the final manifest.json> [--repo G30vamv/Caravan4URecomp] [--no-push]

The branch holds the complete current set of files at each release (git stores identical files once); the commit is tagged
`files-<version>` so that its files stay reachable. Nothing is pushed with --no-push (the URLs then still point to the commit it
would have had, only for a dry run)."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from c4u_updater import validate_manifest  # noqa: E402

NAME = "G30vamv"
EMAIL = "26340841+G30vamv@users.noreply.github.com"


def run(cmd, cwd, check=True):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        sys.exit(f"command failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", required=True)
    ap.add_argument("--package-dir", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--repo", default="G30vamv/Caravan4URecomp")
    ap.add_argument("--no-push", action="store_true")
    args = ap.parse_args()

    package = Path(args.package_dir).resolve()
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="c4u-updates-"))
    try:
        url = f"https://github.com/{args.repo}.git"
        run(["git", "init", "-q", "-b", "updates"], work)
        run(["git", "config", "core.autocrlf", "false"], work)
        run(["git", "config", "user.name", NAME], work)
        run(["git", "config", "user.email", EMAIL], work)
        run(["git", "remote", "add", "origin", url], work)
        if run(["git", "ls-remote", "--heads", "origin", "updates"], work):
            run(["git", "fetch", "-q", "origin", "updates"], work)
            run(["git", "checkout", "-q", "-B", "updates", "origin/updates"], work)
        for entry in work.iterdir():
            if entry.name != ".git":
                shutil.rmtree(entry) if entry.is_dir() else entry.unlink()
        (work / ".gitattributes").write_text("* -text\n", newline="\n")
        for entry in manifest["files"]:
            src = package / entry["path"]
            dst = work / entry["path"]
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
        run(["git", "add", "-A"], work)
        run(["git", "commit", "-q", "-m", f"Update files of Caravan4URecomp {args.version}"], work)
        commit = run(["git", "rev-parse", "HEAD"], work)
        tag = f"files-{args.version}"
        run(["git", "tag", "-f", tag], work)
        if not args.no_push:
            run(["git", "push", "-q", "origin", "updates"], work)
            run(["git", "push", "-q", "-f", "origin", tag], work)
        for entry in manifest["files"]:
            entry["url"] = f"https://raw.githubusercontent.com/{args.repo}/{commit}/" + urllib.parse.quote(entry["path"])
        validate_manifest(manifest, testing=False)
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"updates branch commit {commit} ({len(manifest['files'])} files), tag {tag}; manifest: {out / 'manifest.json'}")
    finally:
        def _writable(function, path, _error):  # git's object files are read-only
            os.chmod(path, 0o700)
            function(path)

        shutil.rmtree(work, onexc=_writable)


if __name__ == "__main__":
    main()

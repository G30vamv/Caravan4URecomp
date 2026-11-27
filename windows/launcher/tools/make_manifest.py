# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Adapted from the update code of the author's other launcher project (updater.py, make_release.py).

"""Prepare the zip, manifest and changed assets of one Windows release."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
from urllib.parse import quote, unquote
import zipfile

try:
    import zlib
except ImportError:
    zlib = None

# Python -I excludes the script directory from sys.path.
if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # c4u_updater.py is in the launcher folder (the parent of tools\)
from c4u_updater import (
    ASSET_NAME_LIMIT, ASSET_SIZE_LIMIT, RELEASE_URL, UPDATE_REPO, UpdateError, _reparse,
    asset_name, load_manifest, parse_version, release_tag, safe_path, sha256_file,
    validate_manifest,
)


def _release_error(en, es):
    return UpdateError("release", en, es)


def _check_size(path):
    size = path.stat().st_size
    if size >= ASSET_SIZE_LIMIT:
        raise _release_error(f"Asset must be smaller than 2 GiB: {path.name}",
                             f"El asset debe ocupar menos de 2 GiB: {path.name}")
    return size


USER_FILES = {"Caravan4URecomp.ini", "launcher_config.json"}  # the player's own settings (the updater protects them)
# made from the player's own game by the first start: a package folder the program was run in has them. (The game's other
# files do not pass asset_name: their kinds are not among those an update may bring.)
GAME_TEXT_FILES = {"monster_names.txt", "item_names.txt", "source.txt"}


def _collect(package):
    paths = []

    def fail(error):
        raise error

    for directory, subdirs, filenames in package.walk(on_error=fail):
        for name in sorted(subdirs + filenames):
            path = directory / name
            relative = path.relative_to(package).as_posix()
            data = path.lstat()
            if _reparse(data) or not path.resolve().is_relative_to(package):
                raise _release_error(f"Linked package path: {relative}",
                                     f"Ruta enlazada en el paquete: {relative}")
            if (name.casefold() == "build" or name.casefold().endswith("_native.dll")
                    or name.casefold() in GAME_TEXT_FILES):
                raise _release_error(f"Private package path: {relative}",
                                     f"Ruta privada en el paquete: {relative}")
            if stat.S_ISDIR(data.st_mode):
                safe_path(f"{relative}/placeholder.txt")
            elif stat.S_ISREG(data.st_mode):
                if relative in USER_FILES:  # neither in the published zip nor in an update: both keep the player's own
                    continue
                asset_name(relative)
                _check_size(path)
                paths.append((relative, path))
            else:
                raise _release_error(f"Expected a regular package file: {relative}",
                                     f"Se esperaba un archivo normal: {relative}")
    return sorted(paths)


def _previous(location, version):
    if location is None:
        return {}
    if location.startswith(("http://", "https://", "file://")):
        # Allow local HTTP(S) fixtures for this release-tool input. Output URLs are checked below.
        data = load_manifest(location, testing=True)
    else:
        data = validate_manifest(json.loads(Path(location).read_text(encoding="utf-8")))
    if parse_version(data["version"]) >= parse_version(version):
        raise _release_error("The previous manifest must be older.",
                             "El manifiesto anterior debe ser de una versión menor.")
    return {entry["path"]: entry for entry in data["files"]}


def build_manifest(version, package_dir, out, prev_manifest=None, notes_en="", notes_es="",
                   min_launcher="0.0.0"):
    tag = release_tag(version)
    parse_version(min_launcher)
    zip_name = f"Caravan4URecomp-{version}-windows-x64.zip"
    if len(zip_name) > ASSET_NAME_LIMIT:
        raise _release_error("The zip asset name is too long.", "El nombre del zip es demasiado largo.")
    package = Path(package_dir)
    if _reparse(package.lstat()) or not package.is_dir():
        raise _release_error("Package directory must be a regular directory.",
                             "La carpeta del paquete debe ser un directorio normal.")
    package = package.resolve(strict=True)
    output = Path(out).resolve()
    if output.is_relative_to(package):
        raise _release_error("Output must be outside the package.",
                             "La salida debe estar fuera del paquete.")
    inputs = _collect(package)
    previous = _previous(prev_manifest, version)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise _release_error("Output directory must be empty.", "La carpeta de salida debe estar vacía.")

    with tempfile.TemporaryDirectory(prefix=".release-", dir=output) as scratch_dir:
        scratch = Path(scratch_dir)
        assets = scratch / "assets"
        assets.mkdir()
        archive = scratch / zip_name
        snapshot = scratch / "snapshot"
        entries = []
        changed = []
        compression = zipfile.ZIP_DEFLATED if zlib else zipfile.ZIP_STORED
        with zipfile.ZipFile(archive, "w", compression) as package_zip:
            for relative, source in inputs:
                # The manifest, archive and release asset all describe these same bytes.
                name = asset_name(relative)
                shutil.copyfile(source, snapshot)
                size = _check_size(snapshot)
                digest = sha256_file(snapshot)
                package_zip.write(snapshot, relative)
                old = previous.get(relative)
                if old and old["sha256"].lower() == digest and old["size"] == size:
                    url = old["url"]
                    prefix = RELEASE_URL + "/download/"
                    # an older release's asset, or a file of the updates branch (publish_updates_branch.py rewrites the URLs of a
                    # published manifest to the raw files of its commit, which stay reachable)
                    if not ((url.startswith(prefix) and _historical_asset(url[len(prefix):], name, version))
                            or _historical_raw(url, relative)):
                        raise _release_error(f"Invalid historical asset URL: {relative}",
                                             f"URL histórica del asset no válida: {relative}")
                    snapshot.unlink()
                else:
                    url = f"{RELEASE_URL}/download/{quote(tag, safe='')}/{name}"
                    snapshot.rename(assets / name)
                    changed.append(name)
                entries.append({"path": relative, "sha256": digest, "size": size, "url": url})

        _check_size(archive)
        data = {
            "version": version, "tag": tag,
            "released": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "notes_en": notes_en, "notes_es": notes_es, "min_launcher": min_launcher,
            "package_zip": {"name": zip_name, "sha256": sha256_file(archive)}, "files": entries,
        }
        manifest = scratch / "manifest.json"
        manifest.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        files = [zip_name, "manifest.json", *(f"assets/{name}" for name in changed)]
        upload = scratch / "UPLOAD.txt"
        upload.write_text(
            f"{tag}\n\n" + "\n".join(files)
            + f'\n\nExample from this directory (PowerShell):\n'
            + '$releaseFiles = @(' + ", ".join(f"'{name}'" for name in files[:2]) + ')\n'
            + "$releaseFiles += @(Get-ChildItem -LiteralPath assets -File | ForEach-Object { $_.FullName })\n"
            + f'gh release upload "{tag}" --repo "{UPDATE_REPO}" @releaseFiles\n',
            encoding="utf-8",
        )
        validate_manifest(data)
        for item in (assets, archive, manifest, upload):
            item.rename(output / item.name)
    return data


def _historical_raw(url, relative):
    """True for https://raw.githubusercontent.com/<update repo>/<40-hex commit>/<this file's path>."""
    prefix = f"https://raw.githubusercontent.com/{UPDATE_REPO}/"
    if not url.startswith(prefix):
        return False
    commit, separator, path = url[len(prefix):].partition("/")
    return bool(separator and re.fullmatch(r"[0-9a-f]{40}", commit) and unquote(path) == relative)


def _historical_asset(suffix, name, version):
    tag, separator, asset = suffix.partition("/")
    if not separator or asset != name or not tag.startswith("windows-v"):
        return False
    old = unquote(tag).removeprefix("windows-v")
    return quote(release_tag(old), safe="") == tag and parse_version(old) < parse_version(version)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--package-dir", required=True)
    parser.add_argument("--prev-manifest")
    parser.add_argument("--out", required=True)
    parser.add_argument("--notes-en", default="")
    parser.add_argument("--notes-es", default="")
    parser.add_argument("--min-launcher", default="0.0.0")
    args = parser.parse_args()
    try:
        data = build_manifest(args.version, args.package_dir, args.out, args.prev_manifest,
                              args.notes_en, args.notes_es, args.min_launcher)
    except (UpdateError, OSError, ValueError, zipfile.LargeZipFile) as error:
        parser.exit(1, f"{error}\n")
    print(f'{data["tag"]}: {len(data["files"])} files, {data["package_zip"]["name"]}')


if __name__ == "__main__":
    main()

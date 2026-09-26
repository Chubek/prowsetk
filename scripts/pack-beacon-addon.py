#!/usr/bin/env python3
"""Pack plugins/beacon's Firefox WebExtension into a deterministic .xpi.

An .xpi is a plain ZIP archive with the extension sources at the root.
The build is deterministic: entries are sorted, timestamps are fixed
(SOURCE_DATE_EPOCH when set, otherwise the manifest mtime), compression
is ZIP_DEFLATED, and Unix modes are normalized to 0644.

Usage:
  pack-beacon-addon.py --source-dir <addon dir> --output <file.xpi>
  pack-beacon-addon.py --source-dir <addon dir> --check <file.xpi>
      [--native-host-manifest <prowsetk_beacon.json>]

--check validates the archive without rebuilding it:
  - valid ZIP with exactly the expected top-level entries
  - manifest.json parses and declares the stable gecko id, the expected
    permissions, and scripts/panel files that are present in the archive
  - when --native-host-manifest is given, its allowed_extensions must
    contain the addon id (Native Messaging allowlist match)
Exit status is 0 on success, non-zero with a message on stderr otherwise.
"""

import argparse
import json
import os
import sys
import zipfile

EXPECTED_FILES = (
    "manifest.json",
    "background.js",
    "content.js",
    "sidebar.html",
    "sidebar.js",
)

EXPECTED_PERMISSIONS = frozenset(
    ["nativeMessaging", "tabs", "webRequest", "<all_urls>", "activeTab"]
)

ADDON_ID = "prowsetk-beacon@example.com"

# Fixed fallback timestamp (YYYY, MM, DD, HH, MM, SS) used when neither
# SOURCE_DATE_EPOCH nor the manifest mtime is available.
FALLBACK_DATE_TIME = (2024, 1, 1, 0, 0, 0)


def fail(message):
    print(f"pack-beacon-addon: error: {message}", file=sys.stderr)
    return 1


def archive_date_time(source_dir):
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        try:
            import time

            return time.gmtime(int(epoch))[:6]
        except ValueError:
            pass
    try:
        import time

        return time.gmtime(int(os.stat(os.path.join(source_dir, "manifest.json")).st_mtime))[:6]
    except OSError:
        return FALLBACK_DATE_TIME


def build(source_dir, output):
    for name in EXPECTED_FILES:
        path = os.path.join(source_dir, name)
        if not os.path.isfile(path):
            return fail(f"missing addon source: {path}")
    date_time = archive_date_time(source_dir)
    tmp_output = output + ".tmp"
    try:
        with zipfile.ZipFile(tmp_output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name in sorted(EXPECTED_FILES):
                with open(os.path.join(source_dir, name), "rb") as handle:
                    data = handle.read()
                info = zipfile.ZipInfo(name, date_time=date_time)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, data)
    except OSError as exc:
        return fail(str(exc))
    try:
        os.replace(tmp_output, output)
    except OSError as exc:
        try:
            os.unlink(tmp_output)
        except OSError:
            pass
        return fail(str(exc))
    return 0


def check(xpi_path, native_host_manifest):
    try:
        with zipfile.ZipFile(xpi_path) as archive:
            names = archive.namelist()
            infos = archive.infolist()
    except (zipfile.BadZipFile, OSError) as exc:
        return fail(f"{xpi_path}: not a valid zip: {exc}")
    if sorted(names) != sorted(EXPECTED_FILES):
        return fail(f"{xpi_path}: unexpected entries: {sorted(names)}")
    for info in infos:
        if info.is_dir() or info.filename.startswith("/") or ".." in info.filename:
            return fail(f"{xpi_path}: unsafe entry: {info.filename}")
    try:
        with zipfile.ZipFile(xpi_path) as archive:
            manifest = json.loads(archive.read("manifest.json").decode("utf-8"))
    except (zipfile.BadZipFile, UnicodeDecodeError, json.JSONDecodeError) as exc:
        return fail(f"{xpi_path}: manifest.json unreadable: {exc}")
    if manifest.get("manifest_version") != 2:
        return fail("manifest.json: manifest_version must be 2")
    for key in ("name", "version"):
        if not manifest.get(key):
            return fail(f"manifest.json: missing {key}")
    gecko = manifest.get("browser_specific_settings", {}).get("gecko", {})
    if gecko.get("id") != ADDON_ID:
        return fail(
            "manifest.json: browser_specific_settings.gecko.id must be "
            f"{ADDON_ID} (must match the native-host allowlist)"
        )
    permissions = manifest.get("permissions", [])
    if not EXPECTED_PERMISSIONS.issubset(set(permissions)):
        return fail(
            f"manifest.json: permissions must include {sorted(EXPECTED_PERMISSIONS)}"
        )
    background = manifest.get("background", {}).get("scripts", [])
    if "background.js" not in background:
        return fail("manifest.json: background.scripts must list background.js")
    panels = (manifest.get("sidebar_action", {}) or {}).get("default_panel")
    if panels != "sidebar.html":
        return fail("manifest.json: sidebar_action.default_panel must be sidebar.html")
    if native_host_manifest:
        try:
            with open(native_host_manifest, encoding="utf-8") as handle:
                host = json.load(handle)
        except (OSError, json.JSONDecodeError) as exc:
            return fail(f"{native_host_manifest}: unreadable: {exc}")
        allowed = host.get("allowed_extensions", [])
        if ADDON_ID not in allowed:
            return fail(
                f"{native_host_manifest}: allowed_extensions must contain {ADDON_ID}"
            )
    print(f"{xpi_path}: OK ({len(names)} files, id {ADDON_ID})")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Pack/validate the Beacon Firefox addon (.xpi).")
    parser.add_argument("--source-dir", default=None)
    parser.add_argument("--output", default=None)
    parser.add_argument("--check", default=None)
    parser.add_argument("--native-host-manifest", default=None)
    args = parser.parse_args(argv)
    if args.check is not None:
        return check(args.check, args.native_host_manifest)
    if not args.source_dir or not args.output:
        parser.error("--source-dir and --output are required (or use --check)")
    return build(args.source_dir, args.output)


if __name__ == "__main__":
    sys.exit(main())

"""Build the LittleFS image source (``data/``) from the web UI source (``web/``).

Every text asset is gzipped; the web server serves ``foo.html`` by looking for
``foo.html.gz`` first and setting ``Content-Encoding: gzip``. This roughly
quarters the transfer size, which matters a lot on an ESP8266.

Assets whose gzipped form is larger than the original (already-compressed
images, tiny files) are copied through verbatim.

Runs as a PlatformIO ``pre:`` extra script, and is also usable standalone:
    python tools/build_web.py --project-dir .
"""

import datetime
import gzip
import io
import hashlib
import json
import os
import shutil
import sys

COMPRESS_EXT = {
    ".html",
    ".htm",
    ".css",
    ".js",
    ".mjs",
    ".json",
    ".svg",
    ".txt",
    ".xml",
    ".webmanifest",
}
SKIP_NAMES = {".DS_Store", "Thumbs.db"}

# Files the *firmware* reads, not only the browser. The firmware has no
# inflater, so these have to stay as they are however well they would compress.
NEVER_COMPRESS = {"devicetypes.json", "manifest.json", "index.json"}

# Only English goes into the image. The rest are fetched by the browser and
# posted to the device when somebody picks one — not to save room, since the
# filesystem is 2 MB and a pack is twenty kilobytes, but so a translation can
# be corrected without anybody reflashing a filesystem. What ships is the
# catalogue: every language that exists, so the chooser can offer one that is
# not here yet.
BUNDLED_LANGUAGES = {"en"}
LANG_DIR = "lang"

# Names the interface's version by its contents, so a filesystem image built
# twice from the same sources gets the same id and one built from different
# sources cannot accidentally share it. The device reports this next to the
# firmware version: the two are flashed separately and do drift apart, and
# "which interface is actually on the device" is otherwise unanswerable.
MANIFEST = "manifest.json"


def _gzip(raw):
    """Compress without stamping the clock into the header.

    gzip.compress() writes the current time into the gzip header, so the same
    input produced different bytes on every run — every file looked changed,
    the filesystem image was rebuilt every time, and so was everything made
    from it. mtime=0 makes a build depend only on its inputs.
    """
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode="wb", compresslevel=9, mtime=0) as fh:
        fh.write(raw)
    return buffer.getvalue()


def _write_if_changed(path, payload):
    """Write only when the bytes differ.

    Rewriting an identical file still moves its timestamp, and a moved
    timestamp is a rebuild — of the filesystem image, and of everything made
    from it.
    """
    if os.path.exists(path):
        with open(path, "rb") as fh:
            if fh.read() == payload:
                return
    with open(path, "wb") as fh:
        fh.write(payload)


def _remove_stale(out_dir, written):
    """Delete anything in data/ that this build did not produce.

    Without the wholesale wipe, a renamed or dropped source file would
    otherwise linger in the image for ever.
    """
    for root, _dirs, files in os.walk(out_dir, topdown=False):
        for name in files:
            path = os.path.join(root, name)
            if os.path.normcase(os.path.abspath(path)) not in written:
                os.remove(path)
                print("build_web: removed stale %s"
                      % os.path.relpath(path, out_dir).replace(os.sep, "/"))
        if root != out_dir and not os.listdir(root):
            os.rmdir(root)


def _write_language_catalogue(src_dir, out_dir):
    """List every language pack in the sources, by code and by its own name.

    The name comes from inside the pack (`lang.name`), so a translator adds a
    language by adding one file and nothing else needs editing.
    """
    lang_src = os.path.join(src_dir, LANG_DIR)
    if not os.path.isdir(lang_src):
        return

    catalogue = []
    for name in sorted(os.listdir(lang_src)):
        code, ext = os.path.splitext(name)
        if ext != ".json" or code == "index":
            continue
        with open(os.path.join(lang_src, name), encoding="utf-8") as fh:
            try:
                strings = json.load(fh)
            except ValueError:
                print("build_web: %s is not valid JSON, skipped" % name)
                continue
        catalogue.append({"code": code, "name": strings.get("lang.name", code)})

    out_lang = os.path.join(out_dir, LANG_DIR)
    os.makedirs(out_lang, exist_ok=True)
    path = os.path.join(out_lang, "index.json")
    _write_if_changed(path, json.dumps(catalogue, ensure_ascii=False,
                                       separators=(",", ":")).encode("utf-8"))
    print("build_web: %d language(s) catalogued, %d bundled"
          % (len(catalogue), len(BUNDLED_LANGUAGES)))
    return path


def build(project_dir):
    src_dir = os.path.join(project_dir, "web")
    out_dir = os.path.join(project_dir, "data")

    if not os.path.isdir(src_dir):
        print("build_web: no web/ directory, nothing to do")
        return

    os.makedirs(out_dir, exist_ok=True)
    written = set()

    total_raw = 0
    total_out = 0
    count = 0
    digest = hashlib.sha1()

    for root, _dirs, files in os.walk(src_dir):
        rel_root = os.path.relpath(root, src_dir)
        dst_root = out_dir if rel_root == "." else os.path.join(out_dir, rel_root)
        os.makedirs(dst_root, exist_ok=True)

        for name in files:
            if name in SKIP_NAMES:
                continue
            if (rel_root == LANG_DIR
                    and os.path.splitext(name)[0] not in BUNDLED_LANGUAGES):
                continue
            src = os.path.join(root, name)
            with open(src, "rb") as fh:
                raw = fh.read()

            # Path as well as content, so moving a file changes the id.
            rel = os.path.relpath(src, src_dir).replace(os.sep, "/")
            digest.update(rel.encode("utf-8") + b"\x00" + raw + b"\x00")

            ext = os.path.splitext(name)[1].lower()
            packed = None
            if ext in COMPRESS_EXT and name not in NEVER_COMPRESS:
                packed = _gzip(raw)

            if packed is not None and len(packed) < len(raw):
                dst = os.path.join(dst_root, name + ".gz")
                payload = packed
            else:
                dst = os.path.join(dst_root, name)
                payload = raw

            _write_if_changed(dst, payload)
            written.add(os.path.normcase(os.path.abspath(dst)))

            total_raw += len(raw)
            total_out += len(payload)
            count += 1

    written.add(os.path.normcase(os.path.abspath(
        _write_language_catalogue(src_dir, out_dir))))

    version = digest.hexdigest()[:8]
    manifest_path = os.path.join(out_dir, MANIFEST)
    written.add(os.path.normcase(os.path.abspath(manifest_path)))

    # The build time only moves when the *contents* do. Stamping it on every
    # run would make one file differ every time, which would rebuild the
    # filesystem image every time, which was the whole thing being fixed.
    previous = {}
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as fh:
            try:
                previous = json.load(fh)
            except ValueError:
                previous = {}
    built = previous.get("built") if previous.get("version") == version else None
    if built is None:
        built = (datetime.datetime.now(datetime.timezone.utc)
                 .strftime("%Y-%m-%dT%H:%M:%SZ"))

    _write_if_changed(manifest_path, json.dumps(
        {"version": version, "built": built,
         "files": count, "bytes": total_out},
        separators=(",", ":")).encode("utf-8"))

    _remove_stale(out_dir, written)

    saved = 100.0 * (1.0 - (total_out / total_raw)) if total_raw else 0.0
    print(
        "build_web: %d files, %d -> %d bytes (%.0f%% smaller), interface %s"
        % (count, total_raw, total_out, saved, version)
    )


if __name__ == "__main__" and "SCons.Script" not in sys.modules:
    project = "."
    if "--project-dir" in sys.argv:
        project = sys.argv[sys.argv.index("--project-dir") + 1]
    build(os.path.abspath(project))
else:
    Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

    build(env.subst("$PROJECT_DIR"))  # noqa: F821

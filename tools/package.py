"""Produce all three images on every build.

    .pio/build/slwf12/firmware.bin          the program            -> 0x000000
    .pio/build/slwf12/littlefs.bin          the web interface      -> 0x200000
    .pio/build/slwf12/slwf12-factory.bin    both, with the gap     -> 0x000000

and a stamped copy of each under dist/, so a week of test builds can be told
apart. The canonical names stay where they are because `pio run -t upload`,
the OTA target and the merge all look for them there; renaming in place would
break every one of them.

`pio run` builds only the program, and `pio run -t buildfs` only the
filesystem — the espressif8266 builder treats them as mutually exclusive
targets, so naming one on the command line suppresses the other. That is a
command-line quirk rather than a real conflict, so this runs the filesystem
build itself once the program is linked, then merges the two.

Both separate images are kept, and they are what an *update* uses: flashing
the factory image over a device in service would erase its configuration,
Wi-Fi credentials and learned infrared codes along with everything else.

Runs as a PlatformIO ``post:`` extra script.
"""

import atexit
import datetime
import os
import re
import shutil
import subprocess
import sys

from SCons.Script import GetBuildFailures

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

PROJECT_DIR = env.subst("$PROJECT_DIR")          # noqa: F821
BUILD_DIR = env.subst("$BUILD_DIR")              # noqa: F821
ENVIRONMENT = env.subst("$PIOENV")               # noqa: F821

# Set while the nested build runs, so the post-action cannot call itself.
GUARD = "SLWF12_PACKAGING"

# How many stamped builds to keep. Enough to bisect a week of testing without
# quietly filling a disk with four-megabyte factory images.
KEEP_BUILDS = 12


def newest(paths):
    times = [os.path.getmtime(p) for p in paths if os.path.exists(p)]
    return max(times) if times else 0.0


def build_filesystem():
    """Run the filesystem target in a child process.

    A child rather than a SCons dependency: the builder decides which of the
    two images to make by looking at the command line, and reaching into that
    decision from here would break the moment the platform is updated.
    """
    environment = dict(os.environ, **{GUARD: "1"})
    result = subprocess.run(
        [sys.executable, "-m", "platformio", "run", "-t", "buildfs",
         "-e", ENVIRONMENT],
        cwd=PROJECT_DIR, env=environment,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print(result.stdout.decode("utf-8", "replace"))
        raise SystemExit("package: the filesystem image failed to build")


def merge(firmware, filesystem, output):
    result = subprocess.run(
        [sys.executable, os.path.join(PROJECT_DIR, "tools", "factory_image.py"),
         "--no-build", "--environment", ENVIRONMENT, "--output", output],
        cwd=PROJECT_DIR,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print(result.stdout.decode("utf-8", "replace"))
        raise SystemExit("package: the factory image could not be assembled")


def sources_newer_than(path):
    """Whether anything in data/ is newer than the given image."""
    if not os.path.exists(path):
        return True
    stamp = os.path.getmtime(path)
    data_dir = os.path.join(PROJECT_DIR, "data")
    for root, _dirs, files in os.walk(data_dir):
        for name in files:
            if os.path.getmtime(os.path.join(root, name)) > stamp:
                return True
    return False


def package():
    if os.environ.get(GUARD):
        return                       # this *is* the nested filesystem build
    if GetBuildFailures():
        return                       # say nothing about a build that failed

    firmware = os.path.join(BUILD_DIR, "firmware.bin")
    filesystem = os.path.join(BUILD_DIR, "littlefs.bin")
    factory = os.path.join(BUILD_DIR, "slwf12-factory.bin")

    if not os.path.exists(firmware):
        return                       # not a build that produced a program

    # Spawning a whole nested build is the expensive part, so it is skipped
    # when the sources it would read have not moved.
    if sources_newer_than(filesystem):
        build_filesystem()

    # mklittlefs and the linker both do their own up-to-date checks; this is
    # the one step with no build system behind it.
    # Not-older, rather than newer: the merge finishes in the same second as
    # the image it just read, and a strict comparison made every build redo it.
    if os.path.exists(factory) and \
            os.path.getmtime(factory) >= newest([firmware, filesystem]):
        print("package: slwf12-factory.bin is up to date")
        return

    merge(firmware, filesystem, factory)

    def kb(path):
        return os.path.getsize(path) // 1024 if os.path.exists(path) else 0

    print("package: firmware.bin %d kB, littlefs.bin %d kB, "
          "slwf12-factory.bin %d kB" % (kb(firmware), kb(filesystem), kb(factory)))

    publish(firmware, filesystem, factory)


def build_version():
    """The version the firmware will report, from the generated header.

    Read rather than recomputed: a name that disagrees with what the device
    prints on boot is worse than no name at all.
    """
    header = os.path.join(PROJECT_DIR, "include", "generated", "version.h")
    try:
        with open(header, encoding="utf-8") as fh:
            found = re.search(r'#define FW_VERSION\s+"([^"]+)"', fh.read())
        if found:
            return found.group(1)
    except OSError:
        pass
    return "unknown"


def interface_version():
    """The digest tools/build_web.py stamped into the filesystem image."""
    manifest = os.path.join(PROJECT_DIR, "data", "manifest.json")
    try:
        with open(manifest, encoding="utf-8") as fh:
            found = re.search(r'"version":"([^"]+)"', fh.read())
        if found:
            return found.group(1)
    except OSError:
        pass
    return "nofs"


def publish(firmware, filesystem, factory):
    """Copy the three images to dist/ under names that identify the build.

    The version alone is not enough while testing: `git describe` returns the
    same string for every uncommitted edit, so two builds an hour apart are
    named identically. The timestamp is what actually tells them apart, and
    the interface digest says whether the web files moved.
    """
    # Timestamp first, and fixed width, so sorting the folder by name sorts it
    # by age. With the version in front, "1.10.0" lands between "1.1.0" and
    # "1.2.0" and the newest build is anywhere at all.
    version = build_version()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    prefix = "slwf12-%s-%s" % (stamp, version)

    out = os.path.join(PROJECT_DIR, "dist")
    os.makedirs(out, exist_ok=True)

    named = [
        (firmware, "%s-firmware.bin" % prefix),
        (filesystem, "%s-littlefs-%s.bin" % (prefix, interface_version())),
        (factory, "%s-factory.bin" % prefix),
    ]
    for source, name in named:
        if os.path.exists(source):
            shutil.copy2(source, os.path.join(out, name))

    prune(out)
    print("package: dist/%s-{firmware,littlefs,factory}.bin" % prefix)


def prune(out):
    """Keep the newest few builds and delete the rest."""
    builds = {}
    for name in os.listdir(out):
        if not name.endswith(".bin"):
            continue
        # Loose on purpose: it has to recognise builds named before the
        # timestamp moved to the front, or they would never be pruned.
        found = re.search(r"(\d{8}-\d{6})", name)
        if found:
            builds.setdefault(found.group(1), []).append(name)

    for stamp in sorted(builds)[:-KEEP_BUILDS]:
        for name in builds[stamp]:
            os.remove(os.path.join(out, name))
        print("package: pruned dist build %s" % stamp)


# Registered to run when the build finishes rather than as a post-action on
# the program image: a build where only the web interface changed does not
# relink the program, and a post-action on something that was not rebuilt
# never fires — which is precisely when the other two images need remaking.
atexit.register(package)

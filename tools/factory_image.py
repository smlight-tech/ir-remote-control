"""Build a single factory image.

    python tools/factory_image.py

produces

    .pio/build/slwf12/slwf12-factory.bin      flash this at 0x000000

Why one file is not the *only* file: the application and the filesystem are not
adjacent in flash. The app ends somewhere below 1 MB, LittleFS starts at 2 MB,
and the gap between them is the OTA staging area that eboot writes an incoming
update into. "One file" therefore means one file that carries that gap as blank
flash, which is what this produces — convenient for first-time flashing, and
about 4 MB.

Keep using the two separate images for *updates*: an OTA firmware update must
not overwrite the filesystem, because the configuration, Wi-Fi credentials and
learned IR codes live there.

This is a standalone script rather than a PlatformIO target because the
espressif8266 builder treats the application and the filesystem image as
mutually exclusive — naming `buildfs` on the command line suppresses the
application build — so no single SCons target can produce both.

Options:
    --environment NAME   PlatformIO environment (default slwf12)
    --no-build           merge what is already in .pio/build, do not rebuild
    --output PATH        write somewhere other than the build directory
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ESP8266 maps flash into the address space at this base.
FLASH_BASE = 0x40200000
ERASED = 0xFF


def run(command):
    print(f"  $ {' '.join(command)}")
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        raise SystemExit(f"factory: `{' '.join(command)}` failed")


def pio():
    """PlatformIO is usually not on PATH when installed with pip --user."""
    for candidate in ("pio", "platformio"):
        try:
            subprocess.run([candidate, "--version"], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           check=True)
            return [candidate]
        except (OSError, subprocess.CalledProcessError):
            continue
    return [sys.executable, "-m", "platformio"]


def filesystem_extent(elf_path, environment):
    """(start, end) of the filesystem as *flash* offsets, read from the ELF, so
    that changing board_build.ldscript cannot silently produce a wrong image."""
    nm = None
    for root, _dirs, files in os.walk(
            os.path.join(os.path.expanduser("~"), ".platformio", "packages",
                         "toolchain-xtensa")):
        for name in files:
            if name.startswith("xtensa-lx106-elf-nm"):
                nm = os.path.join(root, name)
                break
        if nm:
            break
    if nm is None:
        raise SystemExit("factory: could not find xtensa-lx106-elf-nm")

    output = subprocess.check_output([nm, elf_path], stderr=subprocess.DEVNULL)

    symbols = {}
    for line in output.decode("utf-8", "replace").splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("_FS_start", "_FS_end"):
            symbols[parts[2]] = int(parts[0], 16)

    missing = {"_FS_start", "_FS_end"} - set(symbols)
    if missing:
        raise SystemExit(f"factory: {elf_path} has no {', '.join(sorted(missing))}")

    return symbols["_FS_start"] - FLASH_BASE, symbols["_FS_end"] - FLASH_BASE


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--environment", default="slwf12")
    parser.add_argument("--no-build", action="store_true",
                        help="merge what is already built, do not rebuild")
    parser.add_argument("--output")
    args = parser.parse_args()

    build_dir = os.path.join(ROOT, ".pio", "build", args.environment)

    if not args.no_build:
        command = pio()
        run(command + ["run", "-e", args.environment])
        run(command + ["run", "-e", args.environment, "-t", "buildfs"])

    firmware = os.path.join(build_dir, "firmware.bin")
    filesystem = os.path.join(build_dir, "littlefs.bin")
    elf = os.path.join(build_dir, "firmware.elf")
    output = args.output or os.path.join(build_dir, "slwf12-factory.bin")

    for path, hint in ((firmware, "pio run"), (elf, "pio run"),
                       (filesystem, "pio run -t buildfs")):
        if not os.path.exists(path):
            raise SystemExit(f"factory: {path} is missing — run `{hint}` first")

    fs_start, fs_end = filesystem_extent(elf, args.environment)

    with open(firmware, "rb") as handle:
        app = handle.read()
    with open(filesystem, "rb") as handle:
        fs = handle.read()

    if len(app) > fs_start:
        raise SystemExit(
            f"factory: the application is {len(app):,} bytes but the filesystem "
            f"starts at {fs_start:,} — they overlap")
    if len(fs) > fs_end - fs_start:
        raise SystemExit(
            f"factory: the filesystem image is {len(fs):,} bytes but only "
            f"{fs_end - fs_start:,} bytes are reserved for it")

    # Unwritten flash reads as 0xFF, so that is what the gap must contain. A
    # zero-filled gap would be a pointless write and would look, to anything
    # that checks for blank regions, like the OTA area was already used.
    image = bytearray([ERASED]) * fs_end
    image[0:len(app)] = app
    image[fs_start:fs_start + len(fs)] = fs

    with open(output, "wb") as handle:
        handle.write(image)

    blank = fs_end - len(app) - len(fs)
    print()
    print(f"Factory image: {output}")
    print(f"  application  {len(app):>10,} bytes at 0x{0:06X}")
    print(f"  blank / OTA  {blank:>10,} bytes")
    print(f"  filesystem   {len(fs):>10,} bytes at 0x{fs_start:06X}")
    print(f"  total        {len(image):>10,} bytes")
    print()
    print("  esptool.py --chip esp8266 --port COM3 --baud 921600 write_flash \\")
    print("    --flash_mode dio --flash_size 4MB --flash_freq 40m \\")
    print(f"    0x000000 {os.path.basename(output)}")
    print()


if __name__ == "__main__":
    main()

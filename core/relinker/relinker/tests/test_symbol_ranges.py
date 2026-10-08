import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_optional_plt import fixture as optional_plt_fixture

STRINGS = b"\0libfixture.prx\0imported#A#B\0"
NAME_OFFSET = STRINGS.index(b"imported#A#B")


def fixture(extra_tags=()):
    image = optional_plt_fixture(extra_tags)
    struct.pack_into("<H", image, 0x38, 5)
    for index in range(2, 5):
        struct.pack_into("<IIQQQQQQ", image, 64 + index * 56,
                         0x6fffff01, 0, 0, 0, 0, 0, 0, 1)
    return image


def set_tag(image, tag, value, replacement=None):
    for offset in range(0x400, 0x600, 16):
        current, = struct.unpack_from("<q", image, offset)
        if current == tag:
            struct.pack_into("<qQ", image, offset, tag if replacement is None else replacement, value)
            return
        if current == 0:
            break
    raise AssertionError(("missing fixture tag", tag))


def symbol_fixture(os_tag=False, plt=False, relocation_type=6):
    tags = [(1, STRINGS.index(b"libfixture.prx")), (4, 0x680)]
    if plt:
        tags.extend([(3, 0x300), (2, 24), (20, 7), (23, 0x720)])
    image = fixture(tags)
    image[0x600:0x600 + len(STRINGS)] = STRINGS
    set_tag(image, 10, len(STRINGS))
    if os_tag:
        set_tag(image, 6, 0x620, 0x61000039)
        struct.pack_into("<IIQQQQQQ", image, 176,
                         0x61000000, 4, 0, 0, 0, len(image), len(image), 8)
    struct.pack_into("<IIIII", image, 0x680, 1, 2, 1, 0, 0)
    struct.pack_into("<IBBHQQ", image, 0x638, NAME_OFFSET, 0x12, 0, 0, 0, 0)
    struct.pack_into("<QQq", image, 0x720 if plt else 0x700,
                     0x300, (1 << 32) | (7 if plt else relocation_type), 0)
    if plt:
        set_tag(image, 8, 0)
    return image


def main():
    relinker = Path(sys.argv[1]).resolve()
    failures = []
    conversions = 0
    with tempfile.TemporaryDirectory(prefix="anyps5-symbol-ranges-") as directory:
        work = Path(directory)

        def convert(name, image, error=None, relocation_type="R_X86_64_GLOB_DAT", plt=False,
                    windows_error=None):
            nonlocal conversions
            source = work / (name + ".elf")
            source.write_bytes(image)
            for mode in ([], ["--windows"]):
                conversions += 1
                output = work / (name + ("-windows.exe" if mode else "-linux.out"))
                registry = output.with_suffix(".registry.json")
                result = subprocess.run([str(relinker), "--skip-sce-module", "--registry", "unused-filter=0",
                                         *mode, str(source), str(output)],
                                        capture_output=True, text=True, timeout=20)
                if error is not None:
                    valid = result.returncode == 2 and error in result.stderr and not output.exists() and not registry.exists()
                else:
                    if mode and windows_error:
                        valid = (result.returncode == 2 and windows_error in result.stderr
                                 and not output.exists() and registry.exists())
                    else:
                        valid = result.returncode == 0 and output.exists() and registry.exists()
                    if valid:
                        entries = json.loads(registry.read_text())
                        expected = 0 if relocation_type is None else 1
                        valid = len(entries) == expected
                        if valid and expected:
                            entry = entries[0]
                            valid = (entry["nid"] == "imported#A#B"
                                     and entry["relocationType"] == relocation_type
                                     and int(entry["relocationOffset"], 16) == (0x720 if plt else 0x700)
                                     and int(entry["targetOffset"], 16) == 0x300)
                        if output.exists():
                            content = output.read_bytes()
                            valid = valid and content.startswith(b"MZ" if mode else b"\x7fELF")
                            if expected:
                                valid = valid and b"imported\0" in content
                if not valid:
                    failures.append((name, mode, result.returncode, output.exists(), result.stdout, result.stderr))

        convert("unchanged-relative-control", fixture(), relocation_type=None)
        for os_tag in (False, True):
            convert("valid-os" if os_tag else "valid-sysv", symbol_fixture(os_tag))
            convert("valid-absolute-os" if os_tag else "valid-absolute-sysv",
                    symbol_fixture(os_tag, relocation_type=1), relocation_type="R_X86_64_64")

        for plt in (False, True):
            prefix = "plt-" if plt else "rela-"
            relocation_type = "R_X86_64_JUMP_SLOT" if plt else "R_X86_64_GLOB_DAT"
            convert(prefix + "valid-os", symbol_fixture(True, plt), relocation_type=relocation_type, plt=plt)
            convert(prefix + "valid-sysv", symbol_fixture(False, plt), relocation_type=relocation_type, plt=plt)
            for name, base, index in (
                ("base-past-eof", 0x1001, 0),
                ("base-max", (1 << 64) - 1, 0),
                ("base-max-minus-one", (1 << 64) - 2, 0),
                ("indexed-wrap", (1 << 64) - 24 + 0x8, 1),
                ("indexed-wrap-to-valid-name", (1 << 64) - 24 * 0x100 + 0x638, 0x100),
                ("index-max", 0x620, (1 << 32) - 1),
                ("indexed-past-eof", 0xff0, 1),
                ("name-at-eof", 0x1000, 0),
                ("name-one-byte", 0xfff, 0),
                ("name-two-bytes", 0xffe, 0),
                ("name-three-bytes", 0xffd, 0),
            ):
                malformed = symbol_fixture(True, plt)
                set_tag(malformed, 0x61000039, base)
                struct.pack_into("<Q", malformed, (0x720 if plt else 0x700) + 8,
                                 (index << 32) | (7 if plt else 6))
                convert(prefix + name, malformed, "Symbol table entry out of bounds")

            for os_tag in (False, True):
                for readable_bytes in (4, 24):
                    boundary = symbol_fixture(os_tag, plt)
                    set_tag(boundary, 0x61000039 if os_tag else 6, len(boundary) - readable_bytes - 24)
                    struct.pack_into("<I", boundary, len(boundary) - readable_bytes, NAME_OFFSET)
                    convert(prefix + ("os-" if os_tag else "sysv-") + "boundary-" + str(readable_bytes),
                            boundary, relocation_type=relocation_type, plt=plt,
                            windows_error=("Code analysis: file range exceeds image" if os_tag
                                           else "Code analysis: unmapped address") if readable_bytes == 4 else None)

            bad_name = symbol_fixture(True, plt)
            set_tag(bad_name, 10, NAME_OFFSET)
            convert(prefix + "name-outside-strsz", bad_name, "Dynamic string offset is outside DT_STRSZ")
            unterminated = symbol_fixture(True, plt)
            set_tag(unterminated, 10, len(STRINGS) - 1)
            convert(prefix + "unterminated-name", unterminated, "Dynamic string is not NUL-terminated within DT_STRSZ")

    if failures:
        raise AssertionError(failures)
    print(f"Symbol range integration tests passed ({conversions} conversions)")


if __name__ == "__main__":
    main()

import subprocess
import sys
import tempfile
from pathlib import Path

relinker = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)

    def run(*args):
        return subprocess.run([str(relinker), *args], cwd=root, capture_output=True, text=True, timeout=10)

    for args in (
        ("--help",),
        ("-h",),
        ("--windows", "--help"),
        ("--help", "missing.elf", "app.exe", "--autorun", "--registry"),
    ):
        result = run(*args)
        assert result.returncode == 0, result.stderr
        assert result.stderr == "", result.stderr
        for text in ("Usage:", "--windows", "unused-filter=0|1|2", "$ORIGIN/libs", "Deprecated", "Examples:"):
            assert text in result.stdout, result.stdout
        assert not list(root.iterdir()), "Help created output files"

    for args, error in (
        (("--help", "--unknown"), "unknown option"),
        (("--help", "--rpath"), "--rpath requires a value"),
        (("--help", "unused-filter=3"), "unused-filter must be specified once"),
        (("--windows-diagnostics", "input.elf", "app.exe"), "--windows-diagnostics requires --windows"),
        ((), "Usage:"),
    ):
        result = run(*args)
        assert result.returncode == 1, result
        assert result.stdout == "", result.stdout
        assert error in result.stderr, result.stderr

    result = run("missing.elf", "app.elf")
    assert result.returncode == 2, result
    assert not list(root.iterdir()), "Failed conversion created output files"

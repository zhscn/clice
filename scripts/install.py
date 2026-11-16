#!/usr/bin/env python3
"""Install clice and its Clang resource files from a build or release tree."""

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path


def install(source: Path, prefix: Path) -> Path:
    """Copy a bin/lib installation tree, publishing its executable last.

    Resource files must accompany the executable because clice locates its
    builtin headers relative to its own path. Dereferencing source symlinks
    makes the installation independent of Bazel's output and download caches.
    Existing Clang runtime files and other resource versions are preserved.
    """
    source = source.resolve()
    prefix = prefix.absolute()
    binary = next(
        (
            source / "bin" / name
            for name in ("clice.stripped", "clice", "clice.exe")
            if (source / "bin" / name).is_file()
        ),
        None,
    )
    if binary is None:
        raise ValueError(
            f"No bin/clice or bin/clice.exe in {source}; build clice first"
        )
    if prefix.resolve() == source or source in prefix.resolve().parents:
        raise ValueError("Installation prefix must be outside the source tree")

    resources = source / "lib" / "clang"
    includes = sorted(resources.glob("*/include"))
    if not includes:
        raise ValueError(f"No Clang resource headers in {resources}")
    for include in includes:
        for header in ("stddef.h", "stdarg.h", "__stddef_size_t.h"):
            if not (include / header).is_file():
                raise ValueError(f"Missing Clang resource header: {include / header}")

    # Fully copy the input before modifying installed files: a dangling Bazel
    # symlink must fail before a working installation is replaced.
    prefix.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".clice-install-", dir=prefix) as directory:
        staging = Path(directory)
        staged_resources = staging / "clang"
        shutil.copytree(resources, staged_resources, symlinks=False)
        staged_resources.chmod(0o755)
        staged_binary = staging / "clice"
        shutil.copyfile(binary, staged_binary)
        staged_binary.chmod(0o755)

        for entry in sorted(staged_resources.rglob("*")):
            destination = prefix / "lib" / "clang" / entry.relative_to(staged_resources)
            if entry.is_dir():
                entry.chmod(0o755)
                destination.mkdir(parents=True, exist_ok=True)
                destination.chmod(0o755)
            else:
                entry.chmod(0o644)
                os.replace(entry, destination)

        destination = (
            prefix / "bin" / ("clice.exe" if binary.suffix == ".exe" else "clice")
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        os.replace(staged_binary, destination)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(__file__).resolve().parent.parent
        / "build"
        / "RelWithDebInfo"
        / "bin",
        help="tree containing bin/clice and lib/clang (default: repository's RelWithDebInfo build)",
    )
    parser.add_argument(
        "--prefix",
        type=Path,
        default=Path("/usr/local"),
        help="installation prefix (default: /usr/local; use ~/.local for a user installation)",
    )
    args = parser.parse_args()
    try:
        executable = install(args.source.expanduser(), args.prefix.expanduser())
    except (OSError, ValueError, shutil.Error) as error:
        print(f"Installation failed: {error}", file=sys.stderr)
        return 1
    print(f"Installed {executable}")
    print(f"Clang resource files: {executable.parent.parent / 'lib' / 'clang'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# Build from Source

clice builds with [Bazel](https://bazel.build) alone. Bazel downloads everything the build uses: the C++ toolchain and the prebuilt LLVM/Clang libraries of one [xclang](https://github.com/clice-io/xclang) release, clice-io's clang toolchain, and the other libraries clice links. The repository's pixi and npm set up the tools the tests and the docs need; building clice needs neither.

## Requirements

- Bazel, run through [bazelisk](https://github.com/bazelbuild/bazelisk), which fetches the release `.bazelversion` names.
- git and [Node.js](https://nodejs.org) on `PATH`: the build stamps clice with the version `git describe` gives, through a Node script.
- On Windows, Bash (which [Git for Windows](https://gitforwindows.org) includes) and a short Bazel output root, as Bazel's default one is too deep for Windows paths. Put it in `%USERPROFILE%\.bazelrc`:

  ```
  startup --output_user_root=C:/b
  ```

## Building

```shell
bazel build //:bin/clice
```

clice is `build/RelWithDebInfo/bin/bin/clice`, beside the resource directory it reads clang's headers from, `build/RelWithDebInfo/bin/lib/clang`.

The build types are configurations, each with a directory of its own, `build/<type>`:

| Configuration             | Effect                                                                             |
| ------------------------- | ---------------------------------------------------------------------------------- |
| `--config=RelWithDebInfo` | The default: optimized, with debug info                                            |
| `--config=Debug`          | Light optimization (`-O1`) with Address Sanitizer, on x86_64 Linux and arm64 macOS |

`--platforms=@xclang//platforms:<triple>` builds for another target: x86_64 Linux builds every other one, for example `--platforms=@xclang//platforms:aarch64-apple-darwin`.

The LLVM/Clang libraries hold ThinLTO bitcode, so every link of a program redoes their code generation, minutes per program. lld keeps what it generated in a cache, `/var/tmp/xclang-thinlto` (`C:/xclang-thinlto` on Windows), and later links take seconds.

`bazel build //:package //:symbols` builds the release archive and the symbol package, whose `clice.gsym` `scripts/symbolize.py` reads: `clice.tar.gz` and `clice-symbol.tar.xz` in `build/<type>/bin` (`.zip` on Windows).

## Installing

clice locates its builtin Clang headers relative to its executable. Install the executable and its resource files together so that headers such as `stddef.h` remain available:

```shell
bazel build //:bin/clice.stripped
python3 scripts/install.py --prefix ~/.local
```

The installer uses `build/RelWithDebInfo/bin` by default, preferring `bin/clice.stripped` when present and otherwise using `bin/clice`. It installs the executable in `<prefix>/bin` and the resource files in `<prefix>/lib/clang`. Copies follow Bazel's symlinks, so the installed files remain usable after the build cache is removed. Add `<prefix>/bin` to `PATH`, then restart any running clice servers.

For a system installation, run `sudo python3 scripts/install.py --prefix /usr/local`. To install a Debug build or an unpacked release, pass the directory containing both `bin/clice` and `lib/clang` as `--source`, for example `--source build/Debug/bin` or `--source /path/to/unpacked/clice`. The source must contain complete resource files; the installer checks these before changing the installation and replaces the executable after copying the resources. Existing Clang runtime files and other resource versions are preserved.

## Working on clice

`bazel run @compdb//:refresh -- --output_groups=+compilation_prerequisites_INTERNAL_` writes a `compile_commands.json` of clice's own sources to the repository root, for clice to work on its own code; the output group builds the generated headers the commands read.

The tests need the tools pixi installs. `bazel build //:dist` builds clice with the unit tests, then `pixi run test` runs the unit tests and the cases (`--config=Debug` and `pixi run test Debug` for the Debug build); [`tests/README.md`](https://github.com/clice-io/clice/blob/main/tests/README.md) says how a case is written.

## Another LLVM Build

clice links the LLVM/Clang libraries of the exact version it is written for, not the system's. A build of one's own replaces the release's with `--repo_env=XCLANG_LIBCLANG_ROOT=<directory>` (`XCLANG_LIBCLANG_ASAN_ROOT` for the ASan one); it has to be built the way xclang builds it, by xclang's `scripts/toolchain.ts`.

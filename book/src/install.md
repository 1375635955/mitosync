# Install

Download the package for your platform from a GitHub Release.

## The quick way

This works on Linux and macOS. It figures out your platform, installs under `$HOME/.local`, and
never asks for sudo:

```bash
sh -c 'set -eu; os=$(uname -s); arch=$(uname -m); case "$os" in Linux) os=linux;; Darwin) os=macos;; *) echo "unsupported OS: $os" >&2; exit 1;; esac; case "$arch" in x86_64|amd64) arch=x86_64;; aarch64) arch=aarch64;; arm64) [ "$os" = macos ] && arch=arm64 || arch=aarch64;; *) echo "unsupported arch: $arch" >&2; exit 1;; esac; curl -fsSL "https://github.com/echemythia/mitosync/releases/latest/download/mito-$os-$arch.install.sh" | sh'
```

## Or pick your platform yourself

```bash
# Linux x86_64
curl -fsSL https://github.com/echemythia/mitosync/releases/latest/download/mito-linux-x86_64.install.sh | sh

# Linux ARM64
curl -fsSL https://github.com/echemythia/mitosync/releases/latest/download/mito-linux-aarch64.install.sh | sh

# macOS Apple Silicon
curl -fsSL https://github.com/echemythia/mitosync/releases/latest/download/mito-macos-arm64.install.sh | sh
```

macOS releases are **Apple Silicon only**. On an Intel Mac, build from source.

If you would rather download first and run it afterwards, that works the same way:

```bash
sh ./mito-<version>-linux-x86_64.install.sh

# or choose a different prefix you own
sh ./mito-<version>-linux-x86_64.install.sh --prefix "$HOME/.local"
```

The installer puts the binary at `$HOME/.local/bin/mito` and the docs under
`$HOME/.local/share/doc/mito`. Make sure `$HOME/.local/bin` is on your `PATH`.

## System packages on Linux

```bash
sudo apt install ./mito-<version>-linux-x86_64.deb
# or
sudo dnf install ./mito-<version>-linux-x86_64.rpm
```

These install `mito` as `/usr/bin/mito`. Tarballs are also published if you would rather place
things by hand.

## Building from source

You need these first:

| Requirement | Notes |
|-------------|-------|
| CMake 3.19 or newer | Needed for the presets. Without them, 3.15 is the floor |
| A C++17 compiler | GCC, Clang or Apple Clang |
| Ninja | The presets use it. Make works if you configure by hand |
| git | The vcpkg submodule comes down with the clone |

You do not install the libraries yourself. vcpkg handles those.

```bash
git clone --recurse-submodules git@github.com:echemythia/mitosync.git
cd mitosync

./bootstrap.sh                 # one-time: set up vcpkg and fetch dependencies

cmake --preset linux-release   # or macos-release
cmake --build build

./build/mito --version         # prints: mito 0.7.0
./build/mito --help
sudo cmake --install build --prefix /usr/local
```

The version comes from `project(mito VERSION …)` in `CMakeLists.txt`. That is the single source
of truth: the number the binary reports, the number in `vcpkg.json`, and the number in a release
artifact's name are all the same number by construction.

Cloned without `--recurse-submodules`? Fix it with:

```bash
git submodule update --init --recursive
```

## Presets

| Preset | Purpose |
|--------|---------|
| `linux-release` | Optimized Linux build (Ninja) |
| `macos-release` | Optimized macOS build (Ninja) |
| `macos-tsan` | ThreadSanitizer build, for chasing data races |

## What you get

Release builds turn on `-O3`, link-time optimization, `-fstack-protector-strong`, and full RELRO
on Linux. Everything is compiled with `-Wall -Wextra -Wpedantic`.

No architecture flags are set, on purpose. zlib-ng brings its own accelerated CRC32 code and
picks between the versions at run time, based on what the CPU actually reports. So a binary built
on one machine runs, and stays fast, on another.

In `build/` you will find:

- `mito`, the tool
- roughly twenty `mito_tests_*` binaries, one per area

## Dependencies

Resolved by vcpkg from `vcpkg.json`:

| Library | Purpose |
|---------|---------|
| aws-sdk-cpp (s3 only) | S3 API, multipart operations |
| Boost.Asio | Thread pools, async scheduling |
| OpenSSL | TLS for the AWS SDK |
| spdlog | Structured logging |
| nlohmann-json | JSON reports and stats |
| zlib-ng | CRC32, accelerated per CPU at run time |
| zlib | Pulled in by the AWS SDK; the CRC tests use it as an independent reference |
| GoogleTest | Test suite |

## Platform support

| Platform | Architecture | Accelerated CRC32 |
|----------|--------------|-------------------|
| Linux | x86_64 | PCLMULQDQ, or VPCLMULQDQ with AVX-512 |
| Linux | ARM64 | ARMv8 CRC32 |
| macOS | ARM64 (Apple Silicon) | ARMv8 CRC32 |

zlib-ng chooses among these by asking the CPU what it supports. Anything else falls back to
portable C, which is correct but slower. There is no Windows build.

## AWS credentials

`mito` reads credentials from the standard AWS chain and stores none of its own:

1. Environment variables (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`)
2. `~/.aws/credentials` and `~/.aws/config`, including named profiles
3. An ECS task role

You can pick a different profile for each S3 side, which is what makes cross-account work
possible. See [Cross-account S3](reference/cross-account.md).

> **EC2 instance roles do not work.** `mito` disables instance metadata lookup at startup and
> overwrites any value you set. See [Files on disk](reference/files.md#aws-credentials).

## Running the tests

```bash
ctest --test-dir build --output-on-failure      # the whole suite

./build/mito_tests                              # one binary of it
./build/mito_tests --gtest_filter=CRC32Test.*   # one group inside that binary
```

The suite is split across roughly twenty binaries, so `ctest` is the only thing that runs all of
it. `ls build/mito_tests_*` lists them.

**Run the suite serially.** `SyncTaskTest` and `LocalToLocalSyncTest` share fixed temporary
directories and clobber each other under `ctest -j`, producing failures that do not reproduce
when the tests run one at a time.

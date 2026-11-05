# Macman

Macman is a C++17 package manager for macOS with a command-line interface inspired by Arch Linux's `pacman`. It can resolve packages from Homebrew and build source packages from the Arch User Repository (AUR) on macOS.

AUR packages are built from source on the host; Macman does not run Linux binaries through a compatibility layer. Its Self-Healing Engine (SHE) captures build output, recognizes a limited set of known build failures, applies targeted fixes, and retries. It can help with Darwin-specific build issues, but it does not make every AUR package compatible with macOS.

## Features

- Search and resolve packages from Homebrew and the AUR
- Install Homebrew bottles and build AUR packages from source
- Resolve package dependencies and track installed packages in a SQLite database
- Remove packages, find orphan dependencies, and inspect package-owned files
- Verify installed-file integrity with recorded SHA-256 hashes
- Recover from some known build issues using SHE
- Pacman-inspired commands, with a zsh completion script and a man page

## Requirements

- macOS
- Xcode Command Line Tools (including `clang`)
- CMake 3.16 or newer
- Git and curl
- Network access to fetch package metadata and sources

Building the test target also uses CMake `FetchContent` to download Catch2, so the first configure/build needs network access.

## Install

The installer builds Macman from source and installs its executable to `/usr/local/bin/macman`. It creates Macman's data directories under `~/.macman`; package operations that modify the system require `sudo`.

```bash
curl -fsSL https://raw.githubusercontent.com/sairiahzan/macman/main/install.sh | bash
```

The installer requires CMake to already be available. If it is not installed, install it first (for example, with Homebrew). After installation, restart your terminal or reload your shell configuration.

## Build from source

```bash
git clone https://github.com/sairiahzan/macman.git
cd macman
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

Run the locally built program:

```bash
./build/macman --help
```

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

## Commands

| Command | Description |
| --- | --- |
| `macman -Ss <query>` | Search Homebrew and AUR |
| `macman -Si <package>` | Show remote package information |
| `sudo macman -S <package>` | Install a package and resolve dependencies |
| `macman -Sy` | Refresh package databases |
| `sudo macman -Syu` | Refresh databases and upgrade installed packages |
| `macman -Qu` | List available upgrades |
| `macman -Q` | List installed packages |
| `macman -Qi <package>` | Show local package information |
| `macman -Ql <package>` | List files owned by an installed package |
| `macman -Qo <file>` | Show which installed package owns a file |
| `macman -Qk <package>` | Check installed-file integrity |
| `macman -Qt` | List orphan dependencies |
| `macman -T <package>` | Show the installed dependency tree |
| `sudo macman -R <package>` | Remove a package |
| `sudo macman -Rs <package>` | Remove a package and orphan dependencies |
| `sudo macman -Sc` | Clean cached downloads and build files |
| `sudo macman --nuke` | Remove all Macman-managed packages and clean the cache |
| `macman --doctor` | Check selected tools and system configuration |

Use `macman --help` for the complete option list. Review commands carefully before running them, especially `--nuke`.

## AUR builds and SHE

For AUR packages, Macman obtains the package recipe and source, then invokes its build/package steps on macOS. SHE analyzes captured build output and can apply a limited set of known workarounds, such as adding selected compiler flags, addressing certain gettext linking issues, or using GNU coreutils where macOS tools lack expected options. It retries the build after a fix when applicable.

The project maintainer reports successful native macOS builds of `extundelete` and `ext4magic` from AUR source. These are examples, not a guarantee that arbitrary AUR packages will build or work on macOS. Compatibility depends on each package's source, dependencies, and assumptions about Linux APIs and tools. SHE is not a sandbox or Linux compatibility layer.

**Security note:** AUR build recipes are executable shell scripts. Only build packages whose source and recipe you trust. Builds are not isolated in a container.

## Data locations

Macman's runtime configuration and package data are rooted at `~/.macman` (for example, the local database is under `~/.macman/var`). The installer places the executable at `/usr/local/bin/macman`. System-level install/remove operations require elevated permissions.

## Project layout

```text
include/       C++ headers grouped by subsystem
src/
  backend/     Homebrew and AUR integrations
  cli/         Argument parsing
  core/        Database, resolver, transactions, installer, and package logic
  net/         HTTP client and downloads
  ui/          Progress display
tests/         Catch2 unit tests
scripts/       Shell completion
man/           macman(8) manual
third_party/   Bundled nlohmann/json header
```

## Contributing

Issues and pull requests are welcome. Compatibility improvements and tests for package installation, rollback, and platform-specific build behavior are especially useful.

## License

Macman is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).

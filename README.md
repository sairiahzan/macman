# Macman

Macman is a C++17 package manager for macOS inspired by Arch Linux's `pacman` command-line workflow. It brings together access to Homebrew packages and the ability to build AUR source packages natively on macOS.

One of Macman's distinctive features is its **Self-Healing Engine (SHE)**. SHE captures build output, looks for known Darwin compatibility issues, applies targeted fixes, and retries the build. This can make adapting some Linux-oriented source packages to macOS easier. `extundelete` and `ext4magic` are examples of AUR packages that have been built from source and run natively on macOS.

## Features

- Search Homebrew and AUR packages and view package information
- Install Homebrew bottles and build AUR source packages on macOS
- Use SHE to attempt fixes for known build and Darwin compatibility issues
- Resolve dependencies and roll back packages installed in a transaction when an installation fails
- Track packages in a local SQLite database
- Inspect installed packages, their files, and their dependencies
- Check installed-file integrity using SHA-256 hashes
- Pacman-inspired commands, zsh completion, and a man page

## Installation

The installer builds Macman on your machine and places the executable at `/usr/local/bin/macman`. Macman's configuration, database, and cache are stored under `~/.macman`.

Requirements: macOS, Xcode Command Line Tools (`clang`), CMake 3.16 or later, Git, curl, and an internet connection. CMake must already be installed.

```bash
curl -fsSL https://raw.githubusercontent.com/sairiahzan/macman/main/install.sh | bash
```

After installation, restart your terminal or reload your zsh configuration:

```bash
source ~/.zshrc
```

## Build from source

```bash
git clone https://github.com/sairiahzan/macman.git
cd macman
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

Show the help screen:

```bash
./build/macman --help
```

Run the tests:

```bash
ctest --test-dir build --output-on-failure
```

The test target downloads Catch2 through CMake `FetchContent`, so an internet connection is needed during the initial configuration.

## Commands

| Command | Description |
| --- | --- |
| `macman -Ss <query>` | Search Homebrew and AUR |
| `macman -Si <package>` | Show remote package information |
| `sudo macman -S <package>` | Install a package and resolve its dependencies |
| `macman -Sy` | Refresh package databases |
| `sudo macman -Syu` | Refresh databases and upgrade installed packages |
| `macman -Qu` | List packages with available upgrades |
| `macman -Q` | List installed packages |
| `macman -Qi <package>` | Show information about an installed package |
| `macman -Ql <package>` | List files installed by a package |
| `macman -Qo <file>` | Show which package owns a file |
| `macman -Qk <package>` | Check package file integrity |
| `macman -Qt` | List orphaned dependencies |
| `macman -T <package>` | Show the installed dependency tree |
| `sudo macman -R <package>` | Remove a package |
| `sudo macman -Rs <package>` | Remove a package and its orphaned dependencies |
| `sudo macman -Sc` | Clean the cache and build files |
| `sudo macman --nuke` | Remove Macman-managed packages and clean the cache |
| `macman --doctor` | Check selected tools and system settings |

Use `macman --help` to see all available options. Review system-changing commands carefully, especially `--nuke`.

## AUR builds and the Self-Healing Engine

For AUR packages, Macman obtains the package recipe and source code, then runs the build and packaging steps on macOS. SHE examines build logs and attempts targeted workarounds for known issues. Examples implemented in the project include adding selected compiler flags, addressing some gettext linking problems, and using GNU coreutils when macOS tools do not support expected options.

This approach builds from source for macOS rather than running Linux binaries. `extundelete` and `ext4magic` are examples that have been successfully built and run natively. Package results depend on their source code, dependencies, and assumptions about Linux; SHE continues to evolve as more compatibility issues are addressed.

AUR build recipes are executable scripts. Only build packages whose source and recipe you trust. Builds are not currently run inside a container or sandbox.

## Project structure

```text
include/       C++ headers organized by subsystem
src/
  backend/     Homebrew and AUR integrations
  cli/         Command-line argument parsing
  core/        Packages, database, resolution, installation, and transactions
  net/         HTTP client and downloads
  ui/          Progress display
tests/         Catch2 unit tests
scripts/       zsh completion
man/           macman(8) manual
third_party/   nlohmann/json header
```

## Contributing

Contributions are welcome! New Darwin compatibility fixes, documented AUR build successes, and tests for package installation and rollback are especially valuable. Open an issue or pull request on GitHub to report a problem or contribute a change.

## License

Macman is licensed under the GNU General Public License v3.0 (GPL-3.0). See [LICENSE](LICENSE) for details.

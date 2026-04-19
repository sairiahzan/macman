<div align="center">
  <h1>🚀 Macman</h1>

  <p><b>The Blazing-Fast, Self-Healing Package Manager for macOS</b></p>
  <p><i>Written purely in C++17. M-Series Silicon optimized.</i></p>

  <img src="https://img.shields.io/badge/macOS-000000?style=for-the-badge&logo=apple&logoColor=white" alt="macOS" />
  <img src="https://img.shields.io/badge/C%2B%2B17-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white" alt="C++17" />
  <img src="https://img.shields.io/badge/License-GPL_3.0-blue.svg?style=for-the-badge" alt="License GPL 3.0" />
</div>

<br>

**Macman** is not just another package manager for macOS—it is an uncompromising, next-generation build engine. Tired of Homebrew's slow Ruby scripts, endless permission issues, and limited repository? Macman is written 100% in C++17 to deliver instantaneous, multithreaded operations while tapping directly into both the **Homebrew Formulae API** and the legendary **Arch Linux User Repository (AUR)**.

Whether you need pre-compiled macOS binaries or want to natively compile Linux-only hacking tools and bleeding-edge system utilities, Macman is the definitive bridge between macOS elegance and hardcore Linux flexibility.

---

## 🔥 Why Macman? (Killer Features)

- ⚡️ **Blazing Fast Concurrent Execution**: Powered by `std::async`, Macman searches and resolves multiple packages concurrently across Homebrew and AUR APIs. What used to take seconds now takes milliseconds. Unapologetically optimized for M-Series Silicon.
- 🛠️ **Self-Healing Build Engine (SHE)**: Compiling AUR packages on a Mac shouldn't be a nightmare. Macman's intelligent **SHE Motor** now includes:
    - **Automatic Archive Extraction**: Deeply handles `.tar.gz`, `.tar.bz2`, `.tar.xz`, and `.zip` sources automatically, restoring true `makepkg` behavior to macOS.
    - **Intelligent Keg-only Linking**: Automatically detects and injects include/library paths for "keg-only" Homebrew dependencies (like `e2fsprogs`, `openssl`, `icu4c`), solving "header not found" errors before they even happen.
    - **C++ Narrowing Correction**: Detects modern C++11 narrowing conversion errors in legacy code and applies `-Wno-narrowing` patches on the fly.
    - **Darwin API Auto-Patching**: Analyzes build failures due to Linux-specific APIs and auto-stubs or redirects calls to macOS-native equivalents.
- 📦 **Dual Backend Architecture**: Fetches macOS native binaries from Homebrew effortlessly, while also compiling Arch Linux packages from the AUR.
- 🛡️ **User-Space & SIP Friendly**: By default, Macman isolates your environment by installing to `~/.macman`. No more polluting `/usr/local/` or fighting Apple's System Integrity Protection (SIP).
- ☢️ **The Nuke Option**: Want a completely fresh start? A single `macman --nuke` command completely obliterates all installed packages and caches, leaving zeroes and ones as if the system was factory-new.

## 🚀 One-Line Quick Install

You don't need `brew` to install Macman. You don't even need a browser. macOS comes with `curl` out of the box. Open your terminal and summon Macman:

```bash
curl -fsSL https://raw.githubusercontent.com/sairiahzan/macman/main/install.sh | bash
```
*(The script will elegantly clone the repository, compile the C++ binaries natively for your machine, configure your PATH, and clean up after itself).*

> **Note:** Don't forget to restart your terminal or run `source ~/.zshrc` after the installation!

---

## 💻 Commands & Usage

Macman borrows its syntax from Arch Linux's legendary `pacman`, bringing muscle memory to your fingertips.

#### 📦 Package Installation & Search
| Command | Description |
| :--- | :--- |
| `sudo macman -S <pkg>` | Install a package (from Homebrew or AUR). Automatically resolves dependencies. |
| `macman -Ss <query>` | Concurrently search for packages across all databases. |
| `macman -Si <pkg>` | Show detailed information about a package. |

#### 🧹 System Maintenance
| Command | Description |
| :--- | :--- |
| `sudo macman -R <pkg>` | Remove a package. |
| `sudo macman -Rs <pkg>`| Remove a package along with its unused orphan dependencies. |
| `sudo macman --nuke`   | **DANGER:** Nuke the entire system. Removes all packages and cleans caches. |

#### 🔍 Query & Info
| Command | Description |
| :--- | :--- |
| `macman -Q` | List all locally installed packages. |
| `macman -Qi <pkg>`| Display local database information for an installed package. |
| `macman -Ql <pkg>`| List all files owned by an installed package. |

#### 🔄 Database & Updates
| Command | Description |
| :--- | :--- |
| `macman -Sy` | Refresh remote package databases. |
| `sudo macman -Syu` | Full system upgrade (Update databases and upgrade all installed packages). |

---

## 🏗️ Architecture & Workflow

1. **Resolution Phase**: When you request a package, Macman performs a parallelized `O(1)` local hash map search, simultaneously querying Homebrew JSON endpoints and Arch AUR RPC interfaces.
2. **Dependency Graphing**: Recursive dependencies are parsed and ordered before asking for user confirmation. You get a single, clean `[Y/n]` prompt for the entire batch.
3. **Containerized Build System**: When building an AUR package (`PKGBUILD`), Macman provisions a temporary simulated `DESTDIR` container. Instead of letting `makepkg` crash on macOS filesystem limits, Macman dynamically remaps Linux paths (`/usr/bin`, `/usr/share`) to your local user-space prefix (`~/.macman/bin`).
4. **Symlink Remediation**: It intelligently scans deployed artifacts. If a Linux package creates absolute symlinks pointing to `/usr/`, Macman translates and patches them to the correct local environment.

## 🤝 Contribution & License

Pull requests are fiercely welcome. Macman is an evolving giant constructed to push the boundaries of what a package manager can achieve on macOS.

This project is open-source software licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

<div align="center">
  <i>"Fast. Fearless. Native."</i>
</div>

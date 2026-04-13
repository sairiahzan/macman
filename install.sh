#!/bin/bash
# ============================================================================
#  Macman Installer — One-line terminal install (like Homebrew)
# ============================================================================
#  Usage:  
#    bash install.sh              (from project directory)
#    curl -fsSL <url> | bash      (remote install, if hosted)
# ============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

print_banner() {
    echo -e "${CYAN}"
    echo "  ███╗   ███╗ █████╗  ██████╗███╗   ███╗ █████╗ ███╗   ██╗"
    echo "  ████╗ ████║██╔══██╗██╔════╝████╗ ████║██╔══██╗████╗  ██║"
    echo "  ██╔████╔██║███████║██║     ██╔████╔██║███████║██╔██╗ ██║"
    echo "  ██║╚██╔╝██║██╔══██║██║     ██║╚██╔╝██║██╔══██║██║╚██╗██║"
    echo "  ██║ ╚═╝ ██║██║  ██║╚██████╗██║ ╚═╝ ██║██║  ██║██║ ╚████║"
    echo "  ╚═╝     ╚═╝╚═╝  ╚═╝ ╚═════╝╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═══╝"
    echo -e "${NC}"
    echo -e "  ${BOLD}The blazing-fast package manager for macOS${NC}"
    echo ""
}

print_step() {
    echo -e "  ${GREEN}==>${NC} ${BOLD}$1${NC}"
}

print_substep() {
    echo -e "  ${BLUE}  ->${NC} $1"
}

print_error() {
    echo -e "  ${RED}error:${NC} $1" >&2
}

print_warning() {
    echo -e "  ${YELLOW}warning:${NC} $1"
}

# ── Check System ─────────────────────────────────────────────────────────────

check_system() {
    # macOS only
    if [[ "$(uname -s)" != "Darwin" ]]; then
        print_error "macman only runs on macOS."
        exit 1
    fi

    # Check for required tools
    if ! command -v clang &>/dev/null; then
        print_error "Xcode Command Line Tools not installed."
        echo -e "  Run: ${CYAN}xcode-select --install${NC}"
        exit 1
    fi

    if ! command -v cmake &>/dev/null; then
        # Try common paths
        if [[ -f /opt/homebrew/bin/cmake ]]; then
            CMAKE="/opt/homebrew/bin/cmake"
        elif [[ -f /usr/local/bin/cmake ]]; then
            CMAKE="/usr/local/bin/cmake"
        else
            print_error "cmake not found. Install it with: brew install cmake"
            exit 1
        fi
    else
        CMAKE="cmake"
    fi

    # Check for curl (should always be on macOS)
    if ! command -v curl &>/dev/null; then
        print_error "curl not found."
        exit 1
    fi
}

# ── Determine Source ─────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_FROM_SOURCE=false
MACMAN_BINARY=""

# If we're in the project directory with CMakeLists.txt, build from source locally
if [[ -f "$SCRIPT_DIR/CMakeLists.txt" ]] && grep -q "macman" "$SCRIPT_DIR/CMakeLists.txt" 2>/dev/null; then
    BUILD_FROM_SOURCE=true
fi

# Function to clone and build
clone_and_build() {
    print_step "Cloning macman repository..."
    local CLONE_DIR="/tmp/macman_src_$$"
    
    if command -v git &>/dev/null; then
        git clone --depth 1 https://github.com/sairiahzan/macman.git "$CLONE_DIR" || {
            print_error "Failed to clone repository"
            exit 1
        }
    else
        print_error "git is not installed."
        exit 1
    fi

    SCRIPT_DIR="$CLONE_DIR"
    build_from_source
}

# ── Build from Source ────────────────────────────────────────────────────────

build_from_source() {
    print_step "Building macman from source..."

    cd "$SCRIPT_DIR"
    
    # Clean and create build directory
    rm -rf build
    mkdir -p build
    cd build

    print_substep "Configuring with CMake..."
    $CMAKE .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1

    print_substep "Compiling ($(sysctl -n hw.ncpu) threads)..."
    $CMAKE --build . -j$(sysctl -n hw.ncpu) > /dev/null 2>&1

    if [[ ! -f macman ]]; then
        print_error "Build failed."
        exit 1
    fi

    MACMAN_BINARY="$SCRIPT_DIR/build/macman"
    print_substep "Build successful: $(file -b "$MACMAN_BINARY" | head -1)"
}

# ── Install Binary ───────────────────────────────────────────────────────────

install_binary() {
    local INSTALL_DIR="/usr/local/bin"
    local MACMAN_HOME="$HOME/.macman"

    print_step "Installing macman..."

    # Create user-space directories (no sudo needed)
    print_substep "Creating macman directories..."
    mkdir -p "$MACMAN_HOME"/{bin,var/sync,var/cache/builds,etc,lib,include,share,Cellar}

    # Copy binary to /usr/local/bin (needs sudo)
    if [[ -w "$INSTALL_DIR" ]]; then
        cp "$MACMAN_BINARY" "$INSTALL_DIR/macman"
    else
        print_substep "Installing to $INSTALL_DIR (requires sudo)..."
        sudo cp "$MACMAN_BINARY" "$INSTALL_DIR/macman"
        sudo chmod 755 "$INSTALL_DIR/macman"
    fi

    # Add ~/.macman/bin to PATH if not already there
    local SHELL_RC=""
    if [[ -f "$HOME/.zshrc" ]]; then
        SHELL_RC="$HOME/.zshrc"
    elif [[ -f "$HOME/.bashrc" ]]; then
        SHELL_RC="$HOME/.bashrc"
    elif [[ -f "$HOME/.bash_profile" ]]; then
        SHELL_RC="$HOME/.bash_profile"
    fi

    if [[ -n "$SHELL_RC" ]]; then
        if ! grep -q '\.macman/bin' "$SHELL_RC" 2>/dev/null; then
            echo '' >> "$SHELL_RC"
            echo '# macman - package manager for macOS' >> "$SHELL_RC"
            echo 'export PATH="$HOME/.macman/bin:$PATH"' >> "$SHELL_RC"
            print_substep "Added ~/.macman/bin to PATH in $(basename "$SHELL_RC")"
        fi
    fi
}

# ── Verify Installation ─────────────────────────────────────────────────────

verify_install() {
    print_step "Verifying installation..."

    if command -v macman &>/dev/null; then
        local version
        version=$(macman --version 2>/dev/null | grep "macman v" | head -1 | tr -d ' ')
        print_substep "Installed: $version"
    elif [[ -f /usr/local/bin/macman ]]; then
        print_substep "Installed at /usr/local/bin/macman"
    else
        print_warning "Binary installed but may not be in PATH yet."
        print_warning "Run: source ~/.zshrc  (or restart your terminal)"
    fi
}

# ── Print Usage ──────────────────────────────────────────────────────────────

print_usage() {
    echo ""
    echo -e "  ${BOLD}Quick Start:${NC}"
    echo -e "    ${CYAN}sudo macman -S <pkg>${NC}         # Install a package"
    echo -e "    ${CYAN}macman -Ss <query>${NC}           # Search packages"
    echo -e "    ${CYAN}macman -Sy${NC}                   # Sync databases"
    echo -e "    ${CYAN}sudo macman -R <pkg>${NC}         # Remove a package"
    echo -e "    ${CYAN}sudo macman --nuke${NC}           # Wipe all packages completely"
    echo -e "    ${CYAN}macman -Q${NC}                    # List installed"
    echo ""
    echo -e "  ${BOLD}Need help?${NC} ${CYAN}macman --help${NC}"
    echo ""
}

# ── Main ─────────────────────────────────────────────────────────────────────

main() {
    print_banner
    check_system

    if $BUILD_FROM_SOURCE; then
        build_from_source
    else
        # Executed from curl | bash -> clone to /tmp and build
        clone_and_build
    fi

    install_binary
    verify_install

    echo ""
    echo -e "  ${GREEN}✓ macman installed successfully!${NC}"
    print_usage
}

main "$@"

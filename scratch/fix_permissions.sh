#!/bin/bash
# Arda Yiğit - Hazani
# fix_permissions.sh — Resolve ownership issues for macman

MACMAN_DIR="$HOME/.macman"

echo "==> Fixing permissions for $MACMAN_DIR..."

# Change ownership of the entire .macman directory to the current user
if [[ -d "$MACMAN_DIR" ]]; then
    sudo chown -R "$USER":staff "$MACMAN_DIR"
    sudo chmod -R 755 "$MACMAN_DIR"
fi

# Remove macOS security attributes from the binary
if [[ -f "/usr/local/bin/macman" ]]; then
    echo "==> Stripping quarantine/provenance attributes from /usr/local/bin/macman..."
    sudo xattr -d com.apple.provenance /usr/local/bin/macman 2>/dev/null || true
    sudo xattr -d com.apple.quarantine /usr/local/bin/macman 2>/dev/null || true
fi

echo "✓ Done."

#!/usr/bin/env bash
# setup.sh — installs the devkitPro Nintendo DS toolchain environment.
#
# Run this from the project root as root:
#   sudo ./setup.sh
#
# This script:
#   1. Ensures devkitpro-pacman is installed
#   2. Installs the NDS toolchain via dkp-pacman
#   3. Writes persistent environment variables
#   4. Verifies the ARM compiler is available

set -euo pipefail

# ─────────────────────────────────────────────
# 0.  Must be root
# ─────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root (sudo ./setup.sh)" >&2
    exit 1
fi

REAL_USER="${SUDO_USER:-${USER:-root}}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

info()  { echo "  [+] $*"; }
ok()    { echo "  [✓] $*"; }
die()   { echo "  [✗] $*" >&2; exit 1; }
step()  { echo; echo "──── $* ────"; }

# ─────────────────────────────────────────────
# 1.  Confirm devkitpro-pacman is installed
# ─────────────────────────────────────────────
step "Checking devkitpro-pacman"

if ! command -v dkp-pacman &>/dev/null; then
    DKPSCRIPT="$SCRIPT_DIR/devkitpro.sh"

    if [ -f "$DKPSCRIPT" ]; then
        info "devkitpro-pacman not found — running devkitpro.sh first..."
        bash "$DKPSCRIPT" || die "devkitpro.sh failed. Fix errors above and re-run setup.sh."
    else
        die "dkp-pacman not found and devkitpro.sh is not next to setup.sh. Run devkitpro.sh first."
    fi
fi

ok "dkp-pacman is available: $(command -v dkp-pacman)"

# ─────────────────────────────────────────────
# 2.  Install the NDS toolchain
# ─────────────────────────────────────────────
step "Installing NDS toolchain (nds-dev)"

dkp-pacman -Syu --noconfirm       || die "dkp-pacman sync failed"
dkp-pacman -S --noconfirm nds-dev || die "nds-dev install failed"

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITARM="$DEVKITPRO/devkitARM"

[ -d "$DEVKITARM" ]  || die "devkitARM not found at $DEVKITARM"
[ -f "$DEVKITARM/ds_rules" ] || die "ds_rules not found in $DEVKITARM"

ok "devkitARM installed at $DEVKITARM"

# ─────────────────────────────────────────────
# 3.  Write persistent environment variables
# ─────────────────────────────────────────────
step "Writing environment variables to /etc/profile.d/devkitpro.sh"

cat > /etc/profile.d/devkitpro.sh << 'ENVEOF'
# devkitPro environment — written by setup.sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export LIBNDS=/opt/devkitpro/libnds
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
ENVEOF

chmod 644 /etc/profile.d/devkitpro.sh

ok "Written /etc/profile.d/devkitpro.sh"

# Source immediately for current shell
# shellcheck source=/dev/null
source /etc/profile.d/devkitpro.sh

# ─────────────────────────────────────────────
# 4.  Verify compiler availability
# ─────────────────────────────────────────────
step "Verifying arm-none-eabi-gcc"

ARM_GCC="$(command -v arm-none-eabi-gcc 2>/dev/null || true)"

if [ -z "$ARM_GCC" ]; then
    ARM_GCC="$DEVKITARM/bin/arm-none-eabi-gcc"

    [ -x "$ARM_GCC" ] || die "arm-none-eabi-gcc not found even in $DEVKITARM/bin"

    export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
fi

ok "Compiler: $ARM_GCC ($(arm-none-eabi-gcc --version | head -1))"

# ─────────────────────────────────────────────
# 5.  Summary
# ─────────────────────────────────────────────
echo
echo "════════════════════════════════════════════"
echo "  Setup complete!"
echo ""
echo "  Toolchain : $DEVKITARM"
echo "  libnds    : $LIBNDS"
echo "  Compiler  : $(arm-none-eabi-gcc --version | head -1)"
echo ""
echo "  To use the environment in a new shell:"
echo "    source /etc/profile.d/devkitpro.sh"
echo "════════════════════════════════════════════"
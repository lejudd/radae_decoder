#!/bin/bash
#
# package-deb.sh - Build a Debian .deb package for webrx_rade_decode.
#
# Usage:
#   ./package-deb.sh [--skip-build]
#
# Options:
#   --skip-build   Skip the cmake/make step and use the already-built
#                  binary at build/tools/webrx_rade_decode.
#
# Notes:
#   - The build step downloads Opus from GitHub the first time it runs.
#     Subsequent runs reuse the cache at .cache/opus/.
#   - Build-time packages needed:
#       cmake (>= 3.16), gcc, autoconf, automake, libtool, pkg-config,
#       libgtk-3-dev, libhamlib-dev, libpulse-dev
#   - Runtime dependency: libc6 only (all other libs are statically linked).
#
#   Alternately you can, from the top directory do:
#   dpkg-buildpackage -us -uc -b -Ppkg.minimal
#   dpkg-buildpackage -us -uc -b # to do all the binaries

set -euo pipefail

PACKAGE_NAME="webrx-rade-decode"
VERSION="0.1.0"
REVISION="1"
ARCH=$(dpkg --print-architecture)
PKGDIR="${PACKAGE_NAME}_${VERSION}-${REVISION}_${ARCH}"
BUILDDIR="build"
BINARY="${BUILDDIR}/tools/webrx_rade_decode"

# ── Argument parsing ──────────────────────────────────────────────────────────

SKIP_BUILD=false
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        -h|--help)
            sed -n '2,18p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            exit 1
            ;;
    esac
done

# ── Prerequisite checks ───────────────────────────────────────────────────────

for tool in dpkg-deb dpkg-architecture strip install gzip; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: '$tool' is not installed. Install dpkg-dev." >&2
        exit 1
    fi
done

if ! $SKIP_BUILD; then
    for tool in cmake make gcc; do
        if ! command -v "$tool" &>/dev/null; then
            echo "ERROR: '$tool' is not installed." >&2
            exit 1
        fi
    done
fi

# ── Build ─────────────────────────────────────────────────────────────────────

if ! $SKIP_BUILD; then
    echo "=== Configuring cmake build ==="
    mkdir -p "${BUILDDIR}"
    cmake -S . -B "${BUILDDIR}" \
        -DAUDIO_BACKEND=PULSE \
        -DCMAKE_BUILD_TYPE=Release

    echo "=== Building webrx_rade_decode ==="
    cmake --build "${BUILDDIR}" --target webrx_rade_decode -- -j"$(nproc)"
fi

if [ ! -f "${BINARY}" ]; then
    echo "ERROR: Binary not found at '${BINARY}'." >&2
    echo "       Run without --skip-build, or build manually first." >&2
    exit 1
fi

# ── Assemble package directory ────────────────────────────────────────────────

echo "=== Assembling package: ${PKGDIR} ==="
rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}/DEBIAN"
mkdir -p "${PKGDIR}/usr/bin"
mkdir -p "${PKGDIR}/usr/share/doc/${PACKAGE_NAME}"

# Install and strip the binary.
install -m 0755 "${BINARY}" "${PKGDIR}/usr/bin/webrx_rade_decode"
strip --strip-unneeded "${PKGDIR}/usr/bin/webrx_rade_decode"

# Documentation.
install -m 0644 LICENSE "${PKGDIR}/usr/share/doc/${PACKAGE_NAME}/copyright"
gzip --best --no-name -c debian/changelog \
    > "${PKGDIR}/usr/share/doc/${PACKAGE_NAME}/changelog.Debian.gz"

# ── DEBIAN/control ────────────────────────────────────────────────────────────

INSTALLED_SIZE=$(du -sk "${PKGDIR}/usr" | awk '{print $1}')

cat > "${PKGDIR}/DEBIAN/control" <<EOF
Package: ${PACKAGE_NAME}
Version: ${VERSION}-${REVISION}
Architecture: ${ARCH}
Maintainer: Peter B Marks <peter@example.com>
Installed-Size: ${INSTALLED_SIZE}
Depends: libc6 (>= 2.17)
Section: hamradio
Priority: optional
Homepage: https://github.com/peterbmarks/radae_decoder
Description: RADAE streaming audio decoder for OpenWebRX
 webrx-rade-decode reads 16-bit signed mono audio at 8000 Hz from stdin,
 decodes RADAE (Radio Autoencoder) signals, and writes 16-bit signed mono
 audio at 8000 Hz to stdout.
 .
 Designed for integration with OpenWebRX and similar web-based
 software-defined radio (SDR) receiver platforms. Combines a streaming
 Hilbert transform, RADAE RX (OFDM demodulator plus neural decoder), and
 the FARGAN vocoder into a single self-contained command-line tool.
 .
 All neural network weights are compiled into the binary; no external
 model files are required at runtime.
EOF

# ── Build .deb ────────────────────────────────────────────────────────────────

echo "=== Building ${PKGDIR}.deb ==="
dpkg-deb --build --root-owner-group "${PKGDIR}"

echo ""
echo "Done: ${PKGDIR}.deb"
echo "Install with: sudo dpkg -i ${PKGDIR}.deb"

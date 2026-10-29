#!/bin/sh
# CWtty installer for Linux and macOS. Downloads the prebuilt binary of the
# latest release from GitHub and installs it, no compiler needed:
#
#   curl -fsSL https://raw.githubusercontent.com/gslf/CWtty/main/install.sh | sh
#
# Options (pass them with: ... | sh -s -- OPTIONS):
#   --version X   install release X (e.g. 1.0.0) instead of the latest
#   --prefix DIR  install into DIR/bin (default ~/.local)
#   --uninstall   remove cwtty
# CWTTY_VERSION and CWTTY_PREFIX in the environment work too.
# Run it again to update.

set -eu

REPO=gslf/CWtty
version=${CWTTY_VERSION:-latest}
prefix=${CWTTY_PREFIX:-$HOME/.local}
uninstall=0

say() { printf '%s\n' "$*"; }
die() { printf 'cwtty-install: %s\n' "$*" >&2; exit 1; }

usage() {
    say "Usage: install.sh [--version X] [--prefix DIR] [--uninstall]"
    say "  --version X   install release X (e.g. 1.0.0) instead of the latest"
    say "  --prefix DIR  install into DIR/bin (default ~/.local)"
    say "  --uninstall   remove cwtty"
}

while [ $# -gt 0 ]; do
    case $1 in
        --version)   [ $# -ge 2 ] || die "--version needs a value"; version=$2; shift 2 ;;
        --version=*) version=${1#*=}; shift ;;
        --prefix)    [ $# -ge 2 ] || die "--prefix needs a value"; prefix=$2; shift 2 ;;
        --prefix=*)  prefix=${1#*=}; shift ;;
        --uninstall) uninstall=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           printf 'cwtty-install: unknown option %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

os=$(uname -s)
bindir=$prefix/bin
target=$bindir/cwtty

if [ "$os" = Darwin ]; then
    config="$HOME/Library/Application Support/cwtty"
else
    config="${XDG_CONFIG_HOME:-$HOME/.config}/cwtty"
fi

if [ $uninstall = 1 ]; then
    if [ -e "$target" ]; then
        rm -f "$target"
        say "Removed $target"
    else
        say "cwtty is not installed in $bindir"
    fi
    say "Your settings are still in $config (delete it by hand if you want)."
    exit 0
fi

# --- Pick the binary for this system -----------------------------------------

case $os in
    Linux)
        case $(uname -m) in
            x86_64|amd64)  asset=cwtty-linux-x86_64.tar.gz ;;
            aarch64|arm64) asset=cwtty-linux-aarch64.tar.gz ;;
            *) die "no prebuilt binary for Linux $(uname -m): build it from source, see https://github.com/$REPO#building-from-source" ;;
        esac ;;
    Darwin)
        asset=cwtty-macos-universal.tar.gz ;;
    *)
        die "unsupported system $os (on Windows use install.ps1, see https://github.com/$REPO#install)" ;;
esac

case $version in
    latest) base=https://github.com/$REPO/releases/latest/download ;;
    v*)     base=https://github.com/$REPO/releases/download/$version ;;
    *)      base=https://github.com/$REPO/releases/download/v$version ;;
esac

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL --retry 3 -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q -O "$2" "$1"; }
else
    die "curl or wget is needed"
fi

tmp=$(mktemp -d 2>/dev/null || mktemp -d -t cwtty)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' INT TERM

# --- Download and check ------------------------------------------------------

say "Downloading $asset ($version)..."
fetch "$base/$asset" "$tmp/$asset" || die "download failed: $base/$asset"

if fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" 2>/dev/null; then
    expected=$(awk -v f="$asset" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS")
    actual=
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$tmp/$asset" | awk '{ print $1 }')
    elif command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$tmp/$asset" | awk '{ print $1 }')
    fi
    if [ -n "$expected" ] && [ -n "$actual" ] && [ "$expected" != "$actual" ]; then
        die "checksum mismatch for $asset, the download is corrupted"
    fi
fi

mkdir "$tmp/pkg"
tar -xzf "$tmp/$asset" -C "$tmp/pkg" || die "cannot extract $asset"
[ -f "$tmp/pkg/cwtty" ] || die "$asset does not contain cwtty"

# --- Install -----------------------------------------------------------------

mkdir -p "$bindir" 2>/dev/null && [ -w "$bindir" ] ||
    die "cannot write to $bindir: choose another --prefix, or run it with sudo"

# Copy next to the target and rename, so that a running cwtty is not disturbed.
cp "$tmp/pkg/cwtty" "$bindir/.cwtty.$$"
chmod 755 "$bindir/.cwtty.$$"
mv -f "$bindir/.cwtty.$$" "$target"
if [ "$os" = Darwin ]; then
    xattr -d com.apple.quarantine "$target" 2>/dev/null || true
fi

say "Installed $("$target" --version 2>/dev/null || echo cwtty) in $target"

case ":$PATH:" in
    *":$bindir:"*)
        found=$(command -v cwtty 2>/dev/null || true)
        if [ -n "$found" ] && [ "$found" != "$target" ]; then
            say "Warning: $found comes first in your PATH, remove it to use the new one."
        fi
        say "Start it with: cwtty" ;;
    *)
        case ${SHELL:-} in
            */zsh)  rc="$HOME/.zshrc" ;;
            */bash) if [ "$os" = Darwin ]; then rc="$HOME/.bash_profile"; else rc="$HOME/.bashrc"; fi ;;
            *)      rc="$HOME/.profile" ;;
        esac
        say ""
        say "$bindir is not in your PATH. Add it with:"
        say ""
        case ${SHELL:-} in
            */fish) say "  fish_add_path $bindir" ;;
            *)      say "  echo 'export PATH=\"$bindir:\$PATH\"' >> $rc" ;;
        esac
        say ""
        say "then open a new terminal and start it with: cwtty" ;;
esac

#!/usr/bin/env bash
# Installs everything needed to build Lux and use it from VS Code:
#   1. system packages the compiler needs (via apt)
#   2. the compiler itself (build/lux, same as compile.sh)
#   3. the vscode-lux extension, installed into your local VS Code
#
# Safe to re-run: every step is idempotent.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> 1/3: system packages"
if ! command -v apt-get >/dev/null 2>&1; then
    echo "no 'apt-get' on this system -- install these yourself and re-run:"
    echo "  build-essential cmake libsqlite3-dev libpq-dev libmysqlclient-dev"
    echo "  libcairo2-dev libcurl4-openssl-dev libjemalloc-dev nodejs npm"
    exit 1
fi
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake \
    libsqlite3-dev libpq-dev libmysqlclient-dev \
    libcairo2-dev libcurl4-openssl-dev libjemalloc-dev \
    nodejs npm

echo "==> 2/3: building the compiler"
"$HERE/compile.sh"

echo "==> 3/3: the VS Code extension"
EXT_DIR="$HERE/editors/vscode-lux"
if ! command -v code >/dev/null 2>&1; then
    echo "no 'code' CLI on PATH -- skipping the extension (install VS Code and re-run"
    echo "this script, or build/install it by hand: see $EXT_DIR/README.md)."
else
    ( cd "$EXT_DIR" && npm install && npm run compile )
    ( cd "$EXT_DIR/server" && npm install )

    NAME=$(node -p "require('$EXT_DIR/package.json').name")
    PUBLISHER=$(node -p "require('$EXT_DIR/package.json').publisher")
    VERSION=$(node -p "require('$EXT_DIR/package.json').version")

    # `vsce package` needs a newer Node than this machine may have (its
    # dependency `undici` requires >=20); it is tried first because it
    # produces a real, shareable .vsix, but a plain copy into VS Code's
    # extensions folder is every bit a real install and needs nothing but
    # what npm already fetched above, so it is the fallback rather than an
    # error.
    VSIX="$EXT_DIR/$NAME-$VERSION.vsix"
    if (cd "$EXT_DIR" && npx --yes @vscode/vsce package -o "$VSIX") 2>/tmp/lux-vsce.log; then
        code --install-extension "$VSIX" --force
        echo "installed via $VSIX"
    else
        echo "vsce unavailable (see /tmp/lux-vsce.log) -- installing by copying the built"
        echo "extension into VS Code's extensions folder instead."
        TARGET="$HOME/.vscode/extensions/$PUBLISHER.$NAME-$VERSION"
        rm -rf "$HOME/.vscode/extensions/$PUBLISHER.$NAME-"*
        mkdir -p "$TARGET"
        cp -r "$EXT_DIR"/package.json "$EXT_DIR"/language-configuration.json \
              "$EXT_DIR"/README.md "$EXT_DIR"/syntaxes "$EXT_DIR"/out \
              "$EXT_DIR"/node_modules "$TARGET/"
        mkdir -p "$TARGET/server"
        cp -r "$EXT_DIR"/server/out "$EXT_DIR"/server/node_modules "$TARGET/server/"
        echo "installed into $TARGET -- reload VS Code to pick it up"
    fi
fi

echo "==> done: build/lux is ready, and so is the VS Code extension."

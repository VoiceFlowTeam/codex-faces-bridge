#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

cd "$ROOT"
mkdir -p .build/module-cache
export CLANG_MODULE_CACHE_PATH="$ROOT/.build/module-cache"
export SWIFT_MODULECACHE_PATH="$ROOT/.build/module-cache"
swift build -c release --disable-sandbox

BINARY="$ROOT/.build/release/codex-micro-software-bridge"
codesign --force --sign - "$BINARY"
codesign --verify --verbose=2 "$BINARY"
printf '%s\n' "Built probe/self-test binary: $BINARY"
printf '%s\n' "This ad-hoc build cannot create a virtual HID device."

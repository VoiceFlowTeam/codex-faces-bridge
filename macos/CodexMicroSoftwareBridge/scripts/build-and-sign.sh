#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -ne 1 ] || [ "$1" = "-" ]; then
  printf '%s\n' "Usage: $0 'Apple signing identity with virtual HID entitlement'" >&2
  exit 2
fi
IDENTITY=$1

cd "$ROOT"
mkdir -p .build/module-cache
export CLANG_MODULE_CACHE_PATH="$ROOT/.build/module-cache"
export SWIFT_MODULECACHE_PATH="$ROOT/.build/module-cache"
swift build -c release --disable-sandbox

BINARY="$ROOT/.build/release/codex-micro-software-bridge"
codesign --force --sign "$IDENTITY" \
  --entitlements "$ROOT/CodexMicroSoftwareBridge.entitlements" \
  "$BINARY"

codesign --verify --verbose=2 "$BINARY"
codesign -d --entitlements - "$BINARY"
printf '%s\n' "Built and signed: $BINARY"

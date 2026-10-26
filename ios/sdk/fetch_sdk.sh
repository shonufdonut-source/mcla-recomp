#!/usr/bin/env bash
# Clones ReXGlue SDK v0.10.0 into ./sdk and applies this repo's patches plus the iOS ones.
# Run from the repository root. Touches no game files.
set -euo pipefail

SDK_TAG=v0.10.0
SDK_COMMIT=f5337cdc947ff6d4c4196737e2c807a48f2a1fc2
ROOT="$(pwd)"

git clone --recursive --branch "$SDK_TAG" https://github.com/rexglue/rexglue-sdk.git sdk
test "$(git -C sdk rev-parse HEAD)" = "$SDK_COMMIT"

git -C sdk apply "$ROOT/patches/rexglue-v0.10.0-mcla.patch"
git -C sdk apply "$ROOT/patches/rexglue-minmax-blend-factor.patch"

for p in ios/sdk/patches/*.patch; do
  [ -e "$p" ] && git -C sdk apply "$ROOT/$p"
done
# ios/sdk/patches/<name>/*.patch applies inside the submodule sdk/thirdparty/<name>.
for d in ios/sdk/patches/*/; do
  [ -d "$d" ] || continue
  sub="sdk/thirdparty/$(basename "$d")"
  for p in "$d"*.patch; do
    [ -e "$p" ] && git -C "$sub" apply "$ROOT/$p"
  done
done
git -C sdk status --short | head -20

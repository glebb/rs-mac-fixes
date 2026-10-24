#!/bin/sh
# Builds build/rs-mac-fixes: the installer, with the fixes library embedded.
# Needs the Xcode Command Line Tools (xcode-select --install).
set -e
cd "$(dirname "$0")"
mkdir -p build
# Shown by the installer and in the log, so bug reports say which version they are about.
ID="$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"
# The library runs inside the game, which is x86_64 (under Rosetta on Apple Silicon).
# Version 1.0: v1.0.3 and earlier wrote load commands that require 1.0, and macOS before 12 skips
# a library older than its load command requires.
clang -arch x86_64 -mmacosx-version-min=10.13 -O2 -Wall -Wextra -DBUILD_ID="\"$ID\"" -dynamiclib \
    -install_name @executable_path/librsmacfixes.dylib -compatibility_version 1.0 -current_version 1.0 -o build/librsmacfixes.dylib src/rsmacfixes.c
codesign -f -s - build/librsmacfixes.dylib
(cd build && xxd -i librsmacfixes.dylib > payload.h)
clang -arch arm64 -arch x86_64 -mmacosx-version-min=10.13 -O2 -Wall -Wextra -DBUILD_ID="\"$ID\"" -Ibuild \
    -o build/rs-mac-fixes src/installer.c
codesign -f -s - build/rs-mac-fixes
# Releases ship as a zip: a downloaded bare file loses its executable bit.
rm -f build/rs-mac-fixes.zip
ditto -c -k --norsrc --noextattr --noqtn build/rs-mac-fixes build/rs-mac-fixes.zip
echo "built build/rs-mac-fixes and build/rs-mac-fixes.zip"

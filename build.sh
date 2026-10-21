#!/bin/sh
# Builds build/rs-mac-fixes: the installer, with the fixes library embedded.
# Needs the Xcode Command Line Tools (xcode-select --install).
set -e
cd "$(dirname "$0")"
mkdir -p build
# The library runs inside the game, which is x86_64 (under Rosetta on Apple Silicon).
clang -arch x86_64 -mmacosx-version-min=10.13 -O2 -Wall -Wextra -dynamiclib \
    -install_name @executable_path/librsmacfixes.dylib -o build/librsmacfixes.dylib src/rsmacfixes.c
codesign -f -s - build/librsmacfixes.dylib
(cd build && xxd -i librsmacfixes.dylib > payload.h)
clang -arch arm64 -arch x86_64 -mmacosx-version-min=10.13 -O2 -Wall -Wextra -Ibuild \
    -o build/rs-mac-fixes src/installer.c
codesign -f -s - build/rs-mac-fixes
# Releases ship as a zip: a downloaded bare file loses its executable bit.
rm -f build/rs-mac-fixes.zip
ditto -c -k --norsrc --noextattr --noqtn build/rs-mac-fixes build/rs-mac-fixes.zip
echo "built build/rs-mac-fixes and build/rs-mac-fixes.zip"

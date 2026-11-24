#!/bin/sh
set -eu
cd "$(dirname "$0")"
sdk=$(xcrun --show-sdk-path)
case "${1:-read-only}" in
  read-only) variant=read-only; define=-DOMEN_MUX_READ_ONLY ;;
  switch) variant=switch; define=-UOMEN_MUX_READ_ONLY ;;
  *) echo "Usage: $0 [read-only|switch]" >&2; exit 64 ;;
esac
output="build/$variant"
kext="$output/OmenMuxProbe.kext"
bundle="$kext/Contents"
mkdir -p "$bundle/MacOS"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -mkernel -fapple-kext \
  -nostdinc -isystem "$sdk/System/Library/Frameworks/Kernel.framework/Headers" \
  -Wall -Wextra -Werror "$define" -c OmenMuxProbe.cpp -o "$output/OmenMuxProbe.o"
xcrun ld -arch x86_64 -kext -undefined dynamic_lookup -o "$bundle/MacOS/OmenMuxProbe" "$output/OmenMuxProbe.o"
cp Info.plist "$bundle/Info.plist"
plutil -lint "$bundle/Info.plist"
file "$bundle/MacOS/OmenMuxProbe"
codesign --force --sign - "$kext"
codesign --verify --strict "$kext"
xcrun swiftc -warnings-as-errors -o "$output/omen-mux" main.swift MuxService.swift Metrics.swift SMCTemperature.swift
app="$output/OMEN MUX.app"
mkdir -p "$app/Contents/MacOS"
cp AppInfo.plist "$app/Contents/Info.plist"
mkdir -p "$app/Contents/Resources"
if [ -f assets/AppIcon.icns ]; then
  cp assets/AppIcon.icns "$app/Contents/Resources/"
fi
cp "$output/omen-mux" "$app/Contents/MacOS/"
xcrun swiftc -parse-as-library -warnings-as-errors -o "$app/Contents/MacOS/MuxWindow" MuxWindow.swift MuxService.swift Metrics.swift SMCTemperature.swift
codesign --force --sign - "$app/Contents/MacOS/omen-mux"
codesign --force --sign - "$app"
codesign --verify --strict "$app"

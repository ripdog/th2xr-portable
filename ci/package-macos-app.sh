#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <output.dmg>" >&2
  exit 2
fi

output=$1
version=$(cat VERSION)
app="toheart2.app"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp build/toheart2 "$app/Contents/MacOS/"
mkdir -p "$app/Contents/MacOS/shaders/anime4k"
cp build/shaders/anime4k/apply.frag.spv "$app/Contents/MacOS/shaders/anime4k/"

iconset="$app/Contents/Resources/icon.iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
  sips -z "$size" "$size" toheart2.png --out "$iconset/icon_${size}x${size}.png" > /dev/null
  sips -z "$((size * 2))" "$((size * 2))" toheart2.png --out "$iconset/icon_${size}x${size}@2x.png" > /dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/icon.icns"
rm -rf "$iconset"

cat > "$app/Contents/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>toheart2</string>
    <key>CFBundleIdentifier</key>
    <string>com.th2xr-portable.toheart2</string>
    <key>CFBundleName</key>
    <string>ToHeart2 XRATED</string>
    <key>CFBundleIconFile</key>
    <string>icon</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${version}</string>
    <key>CFBundleVersion</key>
    <string>${version}</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
</dict>
</plist>
PLIST

brew_prefix=$(brew --prefix)
frameworks="$app/Contents/Frameworks"
mkdir -p "$frameworks"
shopt -s nullglob

changed=1
while [ "$changed" -eq 1 ]; do
  changed=0
  for file in "$app/Contents/MacOS/toheart2" "$frameworks"/*.dylib; do
    [ -f "$file" ] || continue
    otool -L "$file" | awk '/^\t/ {print $1}' | grep "^$brew_prefix" > /tmp/bundle-deps.txt || true
    while read -r dep; do
      [ -n "$dep" ] || continue
      base=$(basename "$dep")
      if [ ! -f "$frameworks/$base" ] && [ -f "$dep" ]; then
        cp "$dep" "$frameworks/$base"
        chmod u+w "$frameworks/$base"
        changed=1
      fi
    done < /tmp/bundle-deps.txt
  done
done

otool -L "$app/Contents/MacOS/toheart2" | awk '/^\t/ {print $1}' | grep "^$brew_prefix" > /tmp/bundle-deps.txt || true
while read -r dep; do
  base=$(basename "$dep")
  if [ -f "$frameworks/$base" ]; then
    install_name_tool -change "$dep" \
      "@executable_path/../Frameworks/$base" \
      "$app/Contents/MacOS/toheart2"
  fi
done < /tmp/bundle-deps.txt

for dylib in "$frameworks"/*.dylib; do
  [ -f "$dylib" ] || continue
  install_name_tool -id \
    "@executable_path/../Frameworks/$(basename "$dylib")" \
    "$dylib" 2>/dev/null || true
  otool -L "$dylib" | awk '/^\t/ {print $1}' | grep "^$brew_prefix" > /tmp/bundle-deps.txt || true
  while read -r dep; do
    dep_base=$(basename "$dep")
    if [ -f "$frameworks/$dep_base" ]; then
      install_name_tool -change "$dep" \
        "@executable_path/../Frameworks/$dep_base" \
        "$dylib"
    fi
  done < /tmp/bundle-deps.txt
done

hdiutil create -volname "ToHeart2" -srcfolder "$app" -ov -format UDZO "$output"

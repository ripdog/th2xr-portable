#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <output.AppImage>" >&2
  exit 2
fi

output=$1

rm -rf AppDir
mkdir -p AppDir/usr/bin AppDir/usr/share/applications AppDir/usr/share/icons/hicolor/256x256/apps
cp build/toheart2 AppDir/usr/bin/
mkdir -p AppDir/usr/bin/shaders/anime4k
cp build/shaders/anime4k/apply.frag.spv AppDir/usr/bin/shaders/anime4k/

cat > AppDir/usr/share/applications/toheart2.desktop << 'EOF'
[Desktop Entry]
Name=ToHeart2 XRATED
Exec=toheart2
Icon=toheart2
Type=Application
Categories=Game;AdventureGame;
EOF

cp toheart2.png AppDir/usr/share/icons/hicolor/256x256/apps/toheart2.png
ln -s usr/share/icons/hicolor/256x256/apps/toheart2.png AppDir/toheart2.png

wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
chmod +x linuxdeploy-x86_64.AppImage
./linuxdeploy-x86_64.AppImage --appimage-extract-and-run \
  --appdir AppDir --output appimage
mv ToHeart2_XRATED-*.AppImage "$output"

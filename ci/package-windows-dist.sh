#!/usr/bin/env bash
set -euo pipefail

mode=${1:-minimal}

rm -rf dist
mkdir dist
cp build/toheart2.exe dist/
mkdir -p dist/shaders/anime4k
cp build/shaders/anime4k/apply.frag.spv dist/shaders/anime4k/

ldd build/toheart2.exe 2>/dev/null | grep '/ucrt64/bin/' | awk '{print $3}' | while read -r dll; do
  cp "$dll" dist/ 2>/dev/null || true
done

if [ "$mode" = "full" ]; then
  find /ucrt64/bin/ -name '*.dll' | while read -r dll; do
    cp "$dll" dist/ 2>/dev/null || true
  done
fi

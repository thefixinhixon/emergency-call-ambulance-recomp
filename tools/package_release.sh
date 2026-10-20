#!/bin/sh
# Packages a build's run directory for a release.
#
#   tools/package_release.sh <build_dir> <package_name>     (e.g. build eca-linux-x86_64)
#
# Writes <package_name>.tar.gz (Linux) or .zip (when the build holds eca.exe) in the
# current directory. Refuses builds that contain recompiled code: that code is a
# translation of the game's ROM and must not be redistributed.
set -eu
build=$1
name=$2
src=$(cd "$(dirname "$0")/.." && pwd)
run=$build/run

if [ -f "$run/eca.exe" ]; then exe=.exe; else exe=; fi
if [ -f "$build/generated/ppc_table.cpp" ]; then
  echo "package_release: $build was built from a ROM set (generated/ppc_table.cpp exists)" >&2
  echo "package_release: configure a clean build with -DECA_RECOMPILE=OFF" >&2
  exit 1
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
pkg=$stage/$name
mkdir -p "$pkg/Config" "$pkg/ROMs" "$pkg/NVRAM" "$pkg/Saves" "$pkg/tools"
cp "$run/eca$exe" "$pkg/"
cp -r "$run/Assets" "$pkg/"
cp "$run/Config/Games.xml" "$run/Config/Supermodel.ini" "$pkg/Config/"
cp "$build/tools/romtool/romtool$exe" "$pkg/tools/"
cp "$src/tools/build_texture_pack.py" "$src/tools/extract_texture_blocks.py" \
   "$src/tools/upscale_textures.py" "$pkg/tools/"
cp "$src/LICENSE" "$pkg/"
cp "$src/packaging/README.txt" "$pkg/"
echo "Put eca.zip here (see README.txt)." > "$pkg/ROMs/README.txt"

if [ -n "$exe" ]; then
  (cd "$stage" && zip -qr9 "$name.zip" "$name")
  mv "$stage/$name.zip" .
  echo "wrote $name.zip"
else
  tar -C "$stage" -czf "$name.tar.gz" "$name"
  echo "wrote $name.tar.gz"
fi

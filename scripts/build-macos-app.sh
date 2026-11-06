#!/bin/sh
set -eu

usage()
{
    echo "usage: $0 QEMU-CLARION [APP-BUNDLE]" >&2
    exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
binary=$1
binary_dir=$(cd "$(dirname "$binary")" && pwd)
binary="$binary_dir/$(basename "$binary")"
[ -x "$binary" ] || { echo "not executable: $binary" >&2; exit 1; }
app=${2:-"$PWD/Leaf HU Emulator.app"}
script_dir=$(cd "$(dirname "$0")" && pwd)
contents="$app/Contents"
macos="$contents/MacOS"

mkdir -p "$macos" "$contents/Resources"
cp "$script_dir/macos-app/Info.plist" "$contents/Info.plist"
cp "$script_dir/macos-app/AppIcon.icns" "$contents/Resources/AppIcon.icns"
if [ -d "$binary_dir/share/icons" ]; then
    cp -R "$binary_dir/share/icons" "$contents/Resources/"
    ln -sfn "$contents/Resources/share" "$macos/share"
fi
cp "$binary" "$macos/qemu-clarion"
chmod 755 "$macos/qemu-clarion"
if [ -d "$binary_dir/libexec" ]; then
    ln -sfn "$binary_dir/libexec" "$macos/libexec"
else
    for executable in qemu-img qemu-system-arm qemu-system-sh4; do
        if [ -x "$binary_dir/$executable" ]; then
            ln -sfn "$binary_dir/$executable" "$macos/$executable"
        fi
    done
fi
ln -sfn "$binary_dir/lib" "$macos/lib"
ln -sfn "$binary_dir/lib" "$contents/lib"

echo "Created $app"
echo "Pass NOR, optional SD, and QEMU arguments with:"
echo "  open -a \"$app\" --args ..."

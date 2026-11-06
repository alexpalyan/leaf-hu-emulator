#!/bin/sh
set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$script_dir/.." && pwd)
source="$script_dir/macos-app/AppIcon.icns"
icons="$root/ui/icons"

command -v sips >/dev/null 2>&1 || {
    echo "sips is required to generate the platform icons" >&2
    exit 1
}

for size in 16 24 32 48 64 128 256 512; do
    sips -s format png --resampleHeightWidth "$size" "$size" "$source" \
        --out "$icons/leaf-hu-emulator_${size}x${size}.png" >/dev/null
done
sips -s format bmp "$icons/leaf-hu-emulator_32x32.png" \
    --out "$icons/leaf-hu-emulator_32x32.bmp" >/dev/null

python3 "$script_dir/pngs-to-ico.py" \
    "$root/pc-bios/leaf-hu-emulator.ico" \
    "$icons/leaf-hu-emulator_16x16.png" \
    "$icons/leaf-hu-emulator_24x24.png" \
    "$icons/leaf-hu-emulator_32x32.png" \
    "$icons/leaf-hu-emulator_48x48.png" \
    "$icons/leaf-hu-emulator_64x64.png" \
    "$icons/leaf-hu-emulator_128x128.png" \
    "$icons/leaf-hu-emulator_256x256.png"

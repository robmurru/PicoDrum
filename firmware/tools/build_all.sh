#!/usr/bin/env bash
# Builds every shippable firmware variant and collects the UF2s in one folder.
#
# Two independent binary choices, so four builds:
#   panel     1U  = 0.91" 128x32 (1U/20HP)   6HP = 0.96" 128x64 (3U/6HP)
#   encoder   STD = knob turns the right way  REV = the other way
#
# The file names say the Eurorack format, 1U or 3U, not the width: the manual
# and the muR Lab site call the panels that way, and "6HP" next to "1U" mixes a
# width with a height. PANEL=6HP stays the build option's name, only the
# output is renamed. The four names are listed verbatim in the manual and
# carry no version (the release tag does), so they must not change. They
# changed once, picodrum_ -> murdrum_, when the module was renamed (2026-10-01):
#   murdrum_<1U|3U>_enc-<std|rev>.uf2
#
# The encoder direction is a build option and not a setting because the
# firmware cannot tell the two cases apart: the EC11's outer pair is unmarked
# and not consistent between makers, so an otherwise identical part can decode
# CW as CCW. See pins.h. Flash the other one, do not go looking for a fault.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$(cd "$HERE/.." && pwd)/final_builds}"

mkdir -p "$OUT"

for panel in 1U 6HP; do
    for enc in STD REV; do
        echo
        echo "=== $panel / encoder $enc ==============================="
        PANEL="$panel" ENC="$enc" "$HERE/build.sh" "$@"

        src="$HERE/build"
        [[ "$panel" == "6HP" ]] && src="$HERE/build-6hp"
        [[ "$enc"   == "REV" ]] && src="${src}-rev"

        format="$panel"
        [[ "$panel" == "6HP" ]] && format="3U"
        name="murdrum_${format}_enc-$(echo "$enc" | tr 'A-Z' 'a-z').uf2"
        cp "$src/sampleplayer.uf2" "$OUT/$name"
    done
done

echo
echo "=== final_builds ======================================="
ls -l "$OUT"/*.uf2 | awk '{printf "  %-34s %8s bytes\n", substr($NF, match($NF, /[^\/]*$/)), $5}'
echo
echo "Folder: $OUT"

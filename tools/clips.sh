#!/usr/bin/env bash
#
# Run a few of Resolume's bundled demo clips through the plugin at its
# defaults and write one still per clip, three seconds in, for a look.
#
#     tools/clips.sh [out-dir] [--set "Name=value" ...]
#
# Not part of verify.sh: it needs ffmpeg and a Resolume install, and the
# result is a picture to judge by eye -- which is what "the defaults look
# good on real footage" comes down to. The stills are written at 640x360.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-/tmp/splitflap-clips}"
shift || true
mkdir -p "$OUT"
MEDIA="/Applications/Resolume Arena/media"
SFTEST="${SFTEST:-build/sftest}"

for clip in "AV/Beat 001.mov" "AV/Bass 003.mov" "AV/Synth 002.mov" "Shop74/Trinity_09.mov" \
            "Shop74/IntoTheGlow_02.mov" "Shop74/Cyberspace_09.mov" "Shop74/OrganicMotions_06.mov" \
            "Shop74/NeonRoom2_32.mov" "Shop74/FogAndDust_3.mov" "Shop74/Metalive 01.mov"; do
	[ -f "$MEDIA/$clip" ] || continue
	name="$(basename "${clip%.*}" | tr ' ' '_')"
	ffmpeg -v error -i "$MEDIA/$clip" -t 3 -vf "scale=640:360,fps=60" -f rawvideo -pix_fmt rgba - \
		| "$SFTEST" --pipe --size 640x360 --fps 60 --frames 180 "$@" 2>/dev/null \
		| tail -c $(( 640 * 360 * 4 )) \
		| ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s 640x360 -i - "$OUT/$name.png"
	echo "$OUT/$name.png"
done

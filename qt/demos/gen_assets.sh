#!/bin/sh
# HOST: generate the demo assets (H.264 clips + thumbnails) from YOUR OWN video.
#   usage: ./gen_assets.sh <video-file> [output-dir] [--panel WxH] [--popup WxH] [--fps N]
#     --panel WxH  panel size; the cockpit clip is made at exactly this size (default: source video size)
#     --popup WxH  dashboard popup clip size (default: --panel if given, else source video size)
#     --fps N      cap the clip frame rate at N (default: source frame rate, at most 60); never raises it
#   output-dir default: ./demo_assets.   Needs ffmpeg and ffprobe on the host.
# The clip is scaled to cover and center-cropped (no distortion).
# Keep sizes within what the target VPU decodes (1920x1080 or below recommended); sizes should be even numbers.
#
# Output:
#   <out>/videos/cockpit_<W>x<H>.h264     cockpit demo (W x H = panel size)
#   <out>/videos/clip_<W>x<H>_{1,2,3}.h264 dashboard popup clips (3 image boxes; W x H = popup size)
#   <out>/thumbs/ivi_{1,2,3}.png          dashboard image-box thumbnails
set -e
SRC=; OUT=; PANEL=; POPUP=; CAP=
while [ $# -gt 0 ]; do
	case $1 in
	--panel) PANEL=$2; shift 2 ;;
	--popup) POPUP=$2; shift 2 ;;
	--fps) CAP=$2; shift 2 ;;
	-*) echo "unknown option $1"; exit 1 ;;
	*) if [ -z "$SRC" ]; then SRC=$1; else OUT=$1; fi; shift ;;
	esac
done
OUT=${OUT:-./demo_assets}
[ -f "$SRC" ] || { echo "usage: $0 <video-file> [output-dir] [--panel WxH] [--popup WxH] [--fps N]"; exit 1; }
command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "ffmpeg/ffprobe not found on host"; exit 1; }

PROBE=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 "$SRC")
SW=$(echo "$PROBE" | cut -d, -f1); SH=$(echo "$PROBE" | cut -d, -f2); RFR=$(echo "$PROBE" | cut -d, -f3)
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$SRC" | cut -d. -f1)
[ -n "$SW" ] && [ -n "$DUR" ] || { echo "cannot read $SRC"; exit 1; }
FPS=$(awk -v r="$RFR" -v c="${CAP:-60}" 'BEGIN{split(r,a,"/"); f=a[1]/(a[2]?a[2]:1); if(f<1)f=30; if(f>60)f=60; if(c+0>0&&f>c+0)f=c+0; printf "%.3f", f}')

PANEL=${PANEL:-${SW}x${SH}}
POPUP=${POPUP:-$PANEL}
case $PANEL$POPUP in *x*x*) ;; *) echo "sizes must be WxH"; exit 1 ;; esac
echo "source: $SRC (${SW}x${SH} @ $FPS fps, ${DUR}s)  panel: $PANEL  popup: $POPUP"
mkdir -p "$OUT/videos" "$OUT/thumbs"

# name start length WxH
enc() {
	W=${4%x*}; H=${4#*x}
	ffmpeg -y -v error -ss "$2" -t "$3" -i "$SRC" -an \
		-vf "scale=$W:$H:force_original_aspect_ratio=increase,crop=$W:$H,fps=$FPS,setsar=1" \
		-c:v libx264 -preset medium -crf 20 -profile:v high -level 4.2 -pix_fmt yuv420p \
		-f h264 "$OUT/videos/$1.h264"
	echo "  videos/$1.h264"
}

# cockpit: clip size must equal the panel size
enc "cockpit_$PANEL" 0 25 "$PANEL"

# dashboard: three 12 s segments spread over the source, one per image box
STEP=$(( (DUR - 12) / 2 )); [ "$STEP" -lt 0 ] && STEP=0
for n in 1 2 3; do
	ss=$((STEP * (n - 1)))
	enc "clip_${POPUP}_$n" $ss 12 "$POPUP"
	ffmpeg -y -v error -ss $((ss + 1)) -i "$SRC" -frames:v 1 \
		-vf "scale=640:360:force_original_aspect_ratio=increase,crop=640:360" "$OUT/thumbs/ivi_$n.png"
	echo "  thumbs/ivi_$n.png"
done

echo
echo "Done: $OUT"
cp "$(dirname "$0")/run_demo.sh" "$OUT/"
echo "Copy the whole '$OUT' directory to the target board, then run ./run_demo.sh from it on the target (see README.md)."

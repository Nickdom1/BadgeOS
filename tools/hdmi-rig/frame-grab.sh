#!/usr/bin/env bash
# frame-grab.sh [out.png] [WxH] — grab ONE frame from the HDMI capture dongle
# (MS2109/MS2130-class UVC device) on the laptop. Auto-finds the device node,
# skipping the integrated camera.
# Exit codes: 0 = frame written; 2 = no dongle found; 3 = dongle but no frame.
set -u
OUT="${1:-/tmp/badge-frame.png}"
SIZE="${2:-1280x720}"

DEV=""
for d in /sys/class/video4linux/video*; do
  [ -e "$d" ] || continue
  name=$(cat "$d/name" 2>/dev/null)
  case "$name" in
    *Integrated*) continue ;;                       # laptop webcam
    *MACROSILICON*|*MacroSilicon*|*USB\ Video*|*USB3*\ Video*|*MS21*|*Video\ Capture*|*AV\ TO\ USB*)
      DEV="/dev/${d##*/}"; break ;;
  esac
done
# fallback: any non-integrated node
if [ -z "$DEV" ]; then
  for d in /sys/class/video4linux/video*; do
    [ -e "$d" ] || continue
    name=$(cat "$d/name" 2>/dev/null)
    case "$name" in *Integrated*) continue ;; esac
    DEV="/dev/${d##*/}"; break
  done
fi
[ -z "$DEV" ] && { echo "NO-DONGLE: only integrated camera present" >&2; exit 2; }

echo "using $DEV ($(cat /sys/class/video4linux/${DEV##*/}/name 2>/dev/null))" >&2
# The MS2130/MS2109 receiver only asserts HPD (and only locks the incoming TMDS)
# once the USB capture stream is OPEN, and needs ~2-4 s to lock afterward.
# Frame #1 is therefore pre-lock black. Record a short clip, keep a LATE frame.
# Env: SETTLE = seconds to record (default 5).
SETTLE="${SETTLE:-5}"
CLIP="$(dirname "$OUT")/.grab-clip.mkv"
FF="ffmpeg"
command -v ffmpeg >/dev/null 2>&1 || FF="nix run nixpkgs#ffmpeg --"
run_ffmpeg() {
  # $1 = input_format args; record SETTLE seconds, copy MJPEG, pull last frame.
  $FF -hide_banner -loglevel error -f v4l2 $1 -video_size "$SIZE" -i "$DEV" \
      -t "$SETTLE" -c copy -y "$CLIP" \
  && $FF -hide_banner -loglevel error -sseof -1 -i "$CLIP" -frames:v 1 -update 1 -y "$OUT"
}
if run_ffmpeg "-input_format mjpeg" || run_ffmpeg ""; then
  [ -s "$OUT" ] && { rm -f "$CLIP"; echo "$OUT"; exit 0; }
fi
echo "NO-FRAME: dongle at $DEV but could not grab (no HDMI signal?)" >&2
exit 3

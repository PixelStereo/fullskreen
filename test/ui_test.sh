#!/bin/bash
# UI scenario driven by xdotool (Linux + Xvfb + openbox). Usage: test/ui_test.sh [output folder]
# Requirements: Xvfb, openbox, xdotool, ImageMagick; media (make_media.sh) and test projects (make_tests.py).
cd "$(dirname "$0")"
OUT=${1:-out/ui}; mkdir -p "$OUT"
export DISPLAY=:97
Xvfb :97 -screen 0 1920x1080x24 >/dev/null 2>&1 & XV=$!
sleep 1
(openbox >/dev/null 2>&1 &)   # a real window manager: fullscreen, focus, stacking
sleep 1
AUTOSAVE="$HOME/.local/share/Fulskrin/Fulskrin/autosave.fulskrin"
rm -f "$AUTOSAVE"
cp media/h264.mp4 "$OUT/act2_found.mov"
shot() { xwd -root -silent | convert xwd:- "$OUT/$1.png"; }
# Coordinates relative to the main window
X0=0; Y0=0
origin() { # the largest window whose title ends with "Fulskrin" (main window)
    local best=0 w
    for w in $(xdotool search --name "Fulskrin$"); do
        local info ax ay ww hh
        info=$(xwininfo -id "$w")
        ax=$(echo "$info" | awk '/Absolute upper-left X/ {print $4}')
        ay=$(echo "$info" | awk '/Absolute upper-left Y/ {print $4}')
        ww=$(echo "$info" | awk '/Width:/ {print $2}')
        hh=$(echo "$info" | awk '/Height:/ {print $2}')
        if [ $((ww * hh)) -gt $best ]; then best=$((ww * hh)); X0=$ax; Y0=$ay; fi
    done
    echo "main window at $X0,$Y0"
}
at() { xdotool mousemove $((X0 + $1)) $((Y0 + $2)); }
crop() { echo "$3x$4+$((X0 + $1))+$((Y0 + $2))"; }
lum() { convert "$OUT/$1.png" -crop "$2" +repage -colorspace Gray -format '%[fx:mean*255]' info:; }
same() { # number of differing pixels between two screenshots, within an area
    convert "$OUT/$1.png" -crop "$3" +repage /tmp/_a.png; convert "$OUT/$2.png" -crop "$3" +repage /tmp/_b.png
    compare -metric AE /tmp/_a.png /tmp/_b.png null: 2>&1
}
PREVIEW=""

../build/Fulskrin projects/demo.fulskrin >"$OUT/log1.txt" 2>&1 & APP=$!
sleep 5
xdotool key Return; sleep 1                      # "file not found" warning
origin; PREVIEW=$(crop 400 180 700 380)
shot 01_opened; sleep 0.7; shot 01b
echo "animated preview (pixels changed in 0.7 s): $(same 01_opened 01b "$PREVIEW")"

# --- Media Bin: relink the missing file
at 100 100; xdotool click 1; sleep 0.4
at 58 678; xdotool click 1; sleep 1.5
xdotool key ctrl+a; xdotool type --delay 5 "$(realpath "$OUT/act2_found.mov")"; xdotool key Return; sleep 2
shot 02_relinked
at 750 650; xdotool click 1; sleep 0.3
xdotool key ctrl+z; sleep 1.5; shot 03_relink_undone
xdotool key ctrl+shift+z; sleep 1.5; shot 04_relink_redone

# --- Opacity directly in the layer list (Plasma row)
at 1272 783; xdotool mousedown 1; sleep 0.2; at 1240 783; sleep 0.2; at 1205 783; sleep 0.2; xdotool mouseup 1; sleep 0.8
shot 05_opacity
at 750 650; xdotool click 1; sleep 0.3
xdotool key ctrl+z; sleep 1; shot 06_opacity_undone

# --- Drag an image from the Media Bin below the layers: loaded into the selected layer
at 80 144; xdotool mousedown 1; sleep 0.3; at 120 300; sleep 0.3; at 500 880; sleep 0.3; at 600 900; sleep 0.5; xdotool mouseup 1; sleep 1.5
shot 07_dragged

# --- Fullscreen on the main screen (single screen), then back, via keyboard
at 750 650; xdotool click 1; sleep 0.3
xdotool key ctrl+f; sleep 2.5; shot 08_fullscreen
echo "fullscreen: pixels changed in the layers area: $(same 07_dragged 08_fullscreen "$(crop 0 760 1600 150)")"
xdotool key ctrl+f; sleep 2; shot 09_restored
echo "back: pixels changed in the layers area: $(same 07_dragged 09_restored "$(crop 0 760 900 150)")"

# --- Master: blackout with fade, measured in the preview
at 1265 34; xdotool click 1; sleep 1
at 1322 120; xdotool click 1; sleep 2; shot 10_blackout
echo "preview after Blackout: $(lum 10_blackout "$PREVIEW")"
at 1322 120; xdotool click 1; sleep 2; shot 11_lights_up
echo "preview after restore: $(lum 11_lights_up "$PREVIEW")"

# --- Windowed output, autosave, crash, recovery in blackout
xdotool key ctrl+shift+f; sleep 12
ls "$AUTOSAVE" >/dev/null 2>&1 && echo "autosave: yes" || echo "autosave: NO"
kill -9 $APP; sleep 1
../build/Fulskrin >"$OUT/log2.txt" 2>&1 & APP=$!
sleep 4; shot 12_recovery
xdotool key Return; sleep 4; shot 13_restored
kill $APP 2>/dev/null; sleep 1; kill -9 $APP 2>/dev/null
kill $XV
grep -v "XDG\|^OpenGL" "$OUT/log1.txt" "$OUT/log2.txt"
exit 0

#!/bin/bash
# Scénario d'interface piloté par xdotool (Linux + Xvfb + openbox). Usage : test/ui_test.sh [dossier de sortie]
# Prérequis : Xvfb, openbox, xdotool, ImageMagick ; médias (make_media.sh) et projets de test (make_tests.py + demo).
cd "$(dirname "$0")"
OUT=${1:-out/ui}; mkdir -p "$OUT"
export DISPLAY=:97
Xvfb :97 -screen 0 1920x1080x24 >/dev/null 2>&1 & XV=$!
sleep 1
(openbox >/dev/null 2>&1 &)   # un vrai gestionnaire de fenêtres : plein écran, focus, empilement
sleep 1
AUTOSAVE="$HOME/.local/share/Lanterne/Lanterne/autosave.lanterne"
rm -f "$AUTOSAVE"
cp media/h264.mp4 "$OUT/acte2_retrouve.mov"
shot() { xwd -root -silent | convert xwd:- "$OUT/$1.png"; }
# Coordonnées relatives à la fenêtre principale
X0=0; Y0=0
origin() { # la plus grande fenêtre dont le titre se termine par « Lanterne » (fenêtre principale)
    local best=0 w
    for w in $(xdotool search --name "Lanterne$"); do
        local info ax ay ww hh
        info=$(xwininfo -id "$w")
        ax=$(echo "$info" | awk '/Absolute upper-left X/ {print $4}')
        ay=$(echo "$info" | awk '/Absolute upper-left Y/ {print $4}')
        ww=$(echo "$info" | awk '/Width:/ {print $2}')
        hh=$(echo "$info" | awk '/Height:/ {print $2}')
        if [ $((ww * hh)) -gt $best ]; then best=$((ww * hh)); X0=$ax; Y0=$ay; fi
    done
    echo "fenêtre principale en $X0,$Y0"
}
at() { xdotool mousemove $((X0 + $1)) $((Y0 + $2)); }
crop() { echo "$3x$4+$((X0 + $1))+$((Y0 + $2))"; }
lum() { convert "$OUT/$1.png" -crop "$2" +repage -colorspace Gray -format '%[fx:mean*255]' info:; }
same() { # nombre de pixels différents entre deux captures, dans une zone
    convert "$OUT/$1.png" -crop "$3" +repage /tmp/_a.png; convert "$OUT/$2.png" -crop "$3" +repage /tmp/_b.png
    compare -metric AE /tmp/_a.png /tmp/_b.png null: 2>&1
}
PREVIEW=""

../build/Lanterne projects/demo.lanterne >"$OUT/log1.txt" 2>&1 & APP=$!
sleep 5
xdotool key Return; sleep 1                      # avertissement « fichier introuvable »
origin; PREVIEW=$(crop 400 180 700 380)
shot 01_ouvert; sleep 0.7; shot 01b
echo "aperçu animé (pixels changés en 0,7 s) : $(same 01_ouvert 01b "$PREVIEW")"

# --- Chutier : remplacer le fichier introuvable
at 100 129; xdotool click 1; sleep 0.4
at 58 664; xdotool click 1; sleep 1.5
xdotool key ctrl+a; xdotool type --delay 5 "$(realpath "$OUT/acte2_retrouve.mov")"; xdotool key Return; sleep 2
shot 02_remplace
at 750 650; xdotool click 1; sleep 0.3
xdotool key ctrl+z; sleep 1.5; shot 03_remplace_annule
xdotool key ctrl+shift+z; sleep 1.5; shot 04_remplace_retabli

# --- Opacité directement dans la liste des calques (ligne Plasma)
at 1272 783; xdotool mousedown 1; sleep 0.2; at 1240 783; sleep 0.2; at 1205 783; sleep 0.2; xdotool mouseup 1; sleep 0.8
shot 05_opacite
at 750 650; xdotool click 1; sleep 0.3
xdotool key ctrl+z; sleep 1; shot 06_opacite_annulee

# --- Glisser une image du chutier vers la liste des calques
at 80 159; xdotool mousedown 1; sleep 0.3; at 120 300; sleep 0.3; at 500 880; sleep 0.3; at 600 900; sleep 0.5; xdotool mouseup 1; sleep 1.5
shot 07_glisse

# --- Plein écran sur l'écran principal (un seul écran) puis retour, au clavier
at 750 650; xdotool click 1; sleep 0.3
xdotool key ctrl+f; sleep 2.5; shot 08_plein_ecran
echo "plein écran : pixels changés dans la zone des calques : $(same 07_glisse 08_plein_ecran "$(crop 0 760 1600 150)")"
xdotool key ctrl+f; sleep 2; shot 09_retour
echo "retour : pixels changés dans la zone des calques : $(same 07_glisse 09_retour "$(crop 0 760 900 150)")"

# --- Master : noir en fondu, mesuré dans l'aperçu
at 1265 34; xdotool click 1; sleep 1
at 1322 120; xdotool click 1; sleep 2; shot 10_noir
echo "aperçu après Noir : $(lum 10_noir "$PREVIEW")"
at 1322 120; xdotool click 1; sleep 2; shot 11_rallume
echo "aperçu après retour : $(lum 11_rallume "$PREVIEW")"

# --- Sortie fenêtrée, sauvegarde automatique, plantage, reprise au noir
xdotool key ctrl+shift+f; sleep 12
ls "$AUTOSAVE" >/dev/null 2>&1 && echo "sauvegarde automatique : oui" || echo "sauvegarde automatique : NON"
kill -9 $APP; sleep 1
../build/Lanterne >"$OUT/log2.txt" 2>&1 & APP=$!
sleep 4; shot 12_reprise
xdotool key Return; sleep 4; shot 13_restaure
kill $APP 2>/dev/null; sleep 1; kill -9 $APP 2>/dev/null
kill $XV
grep -v "XDG\|^OpenGL" "$OUT/log1.txt" "$OUT/log2.txt"
exit 0

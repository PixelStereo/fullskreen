#!/bin/bash
# Test d'interface piloté par xdotool (Linux + Xvfb). Usage : test/ui_test.sh <dossier de sortie>
OUT=${1:-out/ui}; mkdir -p "$OUT"
export DISPLAY=:97
Xvfb :97 -screen 0 1920x1080x24 >/dev/null 2>&1 & XV=$!
sleep 1
AUTOSAVE="$HOME/.local/share/Lanterne/Lanterne/autosave.lanterne"
rm -f "$AUTOSAVE"
shot() { xwd -root -silent | convert xwd:- "$OUT/$1.png"; }
crop() { convert "$OUT/$1.png" -crop 80x400+1510+300 +repage "$OUT/$1_c.png"; }

../build/Lanterne projects/demo.lanterne >"$OUT/log1.txt" 2>&1 & APP=$!
sleep 5
shot 1_ouvert
# Glisser la poignée haut-gauche de la grille de la mire
xdotool mousemove 322 245 mousedown 1 sleep 0.2 mousemove 350 270 sleep 0.2 mousemove 400 300 sleep 0.2 mouseup 1
sleep 0.5; shot 2_glisse
xdotool key ctrl+z; sleep 0.6; shot 3_annule
xdotool key ctrl+shift+z; sleep 0.6; shot 4_retabli
# Sortie (fenêtrée : même écran)
xdotool key ctrl+shift+f; sleep 2; shot 5_sortie
# Dialogue modal ouvert (Ouvrir… -> « Enregistrer les modifications ? ») : la sortie doit continuer à tourner
xdotool key ctrl+o; sleep 1.5
shot 6a_modal; sleep 1; shot 6b_modal
crop 6a_modal; crop 6b_modal
echo "différence sortie pendant dialogue modal : $(compare -metric AE "$OUT/6a_modal_c.png" "$OUT/6b_modal_c.png" null: 2>&1) pixels"
xdotool key Escape; sleep 1; xdotool mousemove 100 500 click 1; sleep 0.3
# Noir en fondu (1 s)
xdotool key ctrl+b; sleep 2; shot 7_noir; crop 7_noir
echo "luminosité moyenne de la sortie après Noir : $(convert "$OUT/7_noir_c.png" -colorspace Gray -format '%[fx:mean*255]' info:)"
xdotool key ctrl+b; sleep 2; shot 8_retour; crop 8_retour
echo "luminosité moyenne après retour : $(convert "$OUT/8_retour_c.png" -colorspace Gray -format '%[fx:mean*255]' info:)"
# Laisse passer la sauvegarde automatique (10 s), puis plantage simulé
sleep 11
ls -la "$AUTOSAVE" 2>&1 | sed 's/^/autosave : /'
kill -9 $APP; sleep 1
../build/Lanterne >"$OUT/log2.txt" 2>&1 & APP=$!
sleep 4; shot 9_reprise
xdotool key Return; sleep 4; shot 10_restaure; crop 10_restaure
echo "luminosité de la sortie restaurée (attendu : 0, au noir) : $(convert "$OUT/10_restaure_c.png" -colorspace Gray -format '%[fx:mean*255]' info:)"
xdotool key ctrl+b; sleep 2; shot 11_rallume; crop 11_rallume
echo "luminosité après Ctrl+B : $(convert "$OUT/11_rallume_c.png" -colorspace Gray -format '%[fx:mean*255]' info:)"
kill $APP 2>/dev/null; sleep 1; kill -9 $APP 2>/dev/null
kill $XV
grep -v XDG "$OUT/log1.txt" "$OUT/log2.txt"

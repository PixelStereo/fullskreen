# Lanterne — mapping vidéo multi-calques (V1)

Application de mapping vidéo pour la scène et l'installation, Mac / Windows / Linux.
Interface **Qt 6**, décodage **FFmpeg**, rendu **OpenGL 3.3**, effets et générateurs au format **ISF**.

## Ce que fait la V1

- **Calques empilés** (le calque du haut s'affiche au-dessus), opacité, fusion Normal / Addition / Écran / Produit.
- **Une source par calque** :
  - vidéo (H.264, HEVC, ProRes, HAP, DNxHD… tout ce que lit FFmpeg), lecture / pause / boucle / vitesse / position ;
  - image fixe ;
  - générateur ISF.
- **Chaîne d'effets ISF par calque**, en nombre libre, réordonnable, activable effet par effet.
  Paramètres générés automatiquement à partir de l'en-tête JSON de chaque shader.
- **Support ISF v2** : passes multiples, tailles de passe calculées (`"$WIDTH/2"`), tampons persistants et flottants,
  images importées, entrées image secondaires, vertex shaders `.vs`, événements, `IMG_PIXEL` / `IMG_NORM_PIXEL` /
  `IMG_THIS_PIXEL` / `IMG_SIZE`. Bibliothèque scannée dans les dossiers ISF standards + vos dossiers.
- **Mapping par calque** :
  - 4 coins en perspective vraie (homographie) ;
  - grille de déformation de 2×2 à 32×32 points, interpolation lisse (Catmull-Rom) ;
  - les deux se combinent : on cale d'abord les coins, puis on affine à la grille.
- **Sortie plein écran** sur l'écran choisi, cadencée par la synchro verticale. Sur l'écran de l'interface, elle reste fenêtrée.
- **Projets** `.lanterne` (JSON lisible), chemins relatifs au projet pour déplacer un dossier de spectacle d'une machine à l'autre.
- **Glisser-déposer** de vidéos, images, shaders ISF (un filtre déposé s'ajoute comme effet au calque sélectionné).

Shaders fournis (dossier `isf/`) : Mire de calage, Plasma, Dégradé, Nuages, Couleur unie ;
effets Couleur, Teinte, Flou (2 passes), Rémanence (tampon persistant), Kaléidoscope, Bords doux, Masque, Orientation, Pixels.

## Compiler

Il faut Qt 6.2 ou plus récent, FFmpeg (bibliothèques de développement), CMake 3.21+ et un compilateur C++17.

### macOS (Intel ou Apple Silicon)

```bash
brew install qt ffmpeg cmake pkg-config
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j
open build/Lanterne.app
```

Pour distribuer l'application sur une autre machine : `"$(brew --prefix qt)/bin/macdeployqt" build/Lanterne.app`,
puis vérifier avec `otool -L build/Lanterne.app/Contents/MacOS/Lanterne` qu'aucune bibliothèque ne pointe encore vers `/opt/homebrew`.
L'application n'est pas signée : au premier lancement, clic droit → Ouvrir.

### Windows 10/11

1. Installer **Qt 6** (installateur en ligne de qt.io, composant « MSVC 2022 64-bit ») et **Visual Studio 2022** (C++).
2. Télécharger un FFmpeg **shared** avec fichiers de développement, par exemple
   `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` sur github.com/BtbN/FFmpeg-Builds/releases.
   Le dossier décompressé contient `bin/`, `include/`, `lib/`.
3. Dans « x64 Native Tools Command Prompt for VS 2022 » :

```bat
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64 -DFFMPEG_ROOT=C:\ffmpeg-8.1-shared
cmake --build build --config Release
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe build\Release\Lanterne.exe
```

Les DLL FFmpeg sont copiées automatiquement à côté de `Lanterne.exe`. Adaptez les chemins à vos versions.

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev libqt6opengl6-dev libgl1-mesa-dev \
                 libavformat-dev libavcodec-dev libswscale-dev libavutil-dev
cmake -S . -B build && cmake --build build -j
./build/Lanterne
```

## Utilisation

| Action | Commande |
|---|---|
| Nouveau calque | bouton **+** sous la liste, menu Calque, ou glisser un fichier dans la fenêtre |
| Choisir l'écran du vidéoprojecteur | Sortie ▸ Écran de sortie |
| Afficher / masquer la sortie | Ctrl+Maj+F (⌘⇧F sur Mac) |
| Fermer la sortie depuis la sortie | Maj+Échap (Échap seul ne fait rien, par sécurité) |
| Régler la composition sur le projecteur | Sortie ▸ Composition = résolution de l'écran de sortie |
| Lecture / pause du calque vidéo sélectionné | Espace |
| Déplacer une poignée | glisser (Maj : déplacement fin) |
| Déplacer tout le calque | glisser à l'intérieur du calque |
| Ajuster au pixel | flèches (Maj : 10 px) |
| Poignée suivante / désélection | Tab / Échap |
| Mode coins ↔ grille | Inspecteur ▸ Mapping |
| Recharger un shader modifié dans un éditeur | bouton ⟳ dans l'inspecteur |

Méthode de calage conseillée : mettre le générateur **Mire** sur le calque, caler les 4 coins
(les coins colorés indiquent l'orientation : rouge en haut à gauche, vert en haut à droite,
bleu en bas à gauche, jaune en bas à droite), puis passer en mode Grille pour les surfaces non planes.

Dossiers ISF scannés automatiquement : le dossier `isf/` fourni, `/Library/Graphics/ISF` et `~/Library/Graphics/ISF`
sur Mac (là où VDMX et d'autres logiciels installent leurs shaders), plus ceux ajoutés par
Bibliothèque ISF ▸ Ajouter un dossier.

Compatibilité testée avec la collection officielle de Vidvox (dépôt GitHub « ISF-Files ») :
256 des 259 générateurs et effets compilent et se rendent. Les 3 restants (Random Characters,
Tiny Date Time Overlay, Line Group) reposent sur des conversions implicites int/uint que le GLSL standard refuse.
Les 68 transitions ISF sont ignorées : la V1 n'a pas de notion de transition entre médias.

## Architecture

```
src/engine/   moteur, sans aucune dépendance aux widgets (QtCore/QtGui/OpenGL + FFmpeg)
  Engine        composition, rendu, projet JSON, contexte OpenGL hors écran partagé
  Isf           parseur et rendu ISF (traduction GLSL 330 core, passes, tampons)
  VideoDecoder  décodage FFmpeg dans un thread, file d'images, boucle sans couture, positionnement
  Mapping       homographie 4 coins + grille Catmull-Rom
src/ui/       interface Qt Widgets
  MainWindow, LayerInspector, ParamPanel, MappingView (édition), OutputWindow (projecteur)
isf/          shaders fournis
test/         tests automatiques
```

Le moteur rend la composition dans une texture ; la vue d'édition et la fenêtre de sortie ne font que l'afficher.
On peut donc remplacer l'interface, ou piloter le moteur en OSC, sans toucher au rendu.

## Tests

```bash
test/make_media.sh                         # médias de test, une fois
cmake --build build --target lanterne_tests
./build/lanterne_tests                     # chargement/sauvegarde, vidéo, ISF
./build/Lanterne --render sortie.png projet.lanterne   # rendu sans interface d'un projet
```

`test/make_media.sh` crée les médias de test avec FFmpeg (H.264, ProRes, HAP, image), `test/make_tests.py`
génère un projet par shader et par codec. `lanterne_tests --check-isf <dossier>` compile et rend tous les shaders d'un dossier.

## Limites connues de la V1 (et suite logique)

- **Décodage CPU vers RGBA** : confortable pour quelques flux HD. Pour plusieurs flux 4K il faudra :
  conversion YUV→RVB sur le GPU, envoi direct des textures HAP compressées (DXT) au GPU,
  décodage matériel (VideoToolbox sur Mac, D3D11VA sur Windows, VAAPI sur Linux).
- **Pas d'audio.**
- **Une seule sortie** : pas encore de sorties multiples, de découpe par projecteur ni d'edge blending automatique
  (l'effet « Bords doux » dépanne pour un blending manuel).
- **Pas de timeline, de cues ni de pilotage OSC / MIDI / DMX.**
- **Pas d'entrées Syphon / Spout / NDI, ni de caméra.**
- Entrées audio des shaders ISF non gérées (texture noire).
- Rendu dans le thread de l'interface, synchronisé par `glFinish` : simple et fiable, mais un dialogue modal fige la sortie.
  Un thread de rendu dédié est l'étape suivante avant une utilisation en représentation.

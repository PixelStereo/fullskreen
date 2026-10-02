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
- **Sortie plein écran** sur l'écran choisi, cadencée par la synchro verticale : **⌘F / Ctrl+F** pour entrer et sortir
  du plein écran (sur le second écran s'il est branché, sinon sur l'écran principal), aussi depuis la sortie elle-même.
  ⌘⇧F / Ctrl+Maj+F : sortie dans une fenêtre.
- **Rendu dans un fil dédié** : le moteur rend et présente la sortie lui-même ; un ralentissement ou un blocage
  de l'interface (chargement, dialogue, menu) n'interrompt pas l'image projetée.
- **Onglet Master** (à côté de l'onglet Calque) : fader de niveau général et bouton **Noir** en fondu
  (durée réglable, Ctrl+B / ⌘B, actif même depuis la sortie), écran et mode de sortie, taille de la composition,
  publication de la sortie.
- **Publication de la sortie** vers d'autres logiciels ou machines :
  - **NDI** (réseau) : nécessite NDI Tools ou le NDI Runtime (ndi.video) installé sur la machine ;
  - **OMT** — Open Media Transport (réseau, libre) : nécessite `libomt` et `libvmx`
    (binaires sur github.com/openmediatransport/libomtnet/releases) à côté de l'application,
    dans `/usr/local/lib`, ou dans le dossier indiqué dans l'onglet Master ;
  - **Syphon** (macOS) et **Spout** (Windows) : partage direct de la texture sur la carte graphique,
    compilés avec Lanterne (aucune installation).
  NDI et OMT sont chargés au moment de l'activation : Lanterne fonctionne sans eux. L'onglet Master indique
  pour chacun l'état (actif, nombre de récepteurs, ou ce qui manque). Les réglages sont enregistrés dans le projet.
- **Chutier** (à gauche) : tous les fichiers images et vidéos du projet, classés par type, avec résolution,
  durée et nombre de calques qui les utilisent (sources et images des shaders). Fichiers **introuvables** en rouge,
  **Remplacer…** pour les retrouver (un seul remplacement corrige tous les calques, annulable) ;
  import sans créer de calque, glisser vers la liste des calques ou l'aperçu pour créer un calque,
  double-clic pour remplacer la source du calque sélectionné.
- **Liste des calques en bas**, sur toute la largeur : visibilité, nom, source, effets, opacité réglable
  directement dans la ligne, fusion, position de lecture.
- **Annuler / rétablir** (Ctrl+Z / Ctrl+Maj+Z, ⌘ sur Mac) : mapping (poignées, flèches, boutons), paramètres ISF,
  opacité, fusion, visibilité, nom, ajout / suppression / ordre des calques, effets, changement de source.
- **Sauvegarde automatique et reprise** : la session est sauvegardée toutes les 10 s quand elle change.
  Après un plantage, Lanterne propose de la restaurer au lancement ; si la sortie était affichée,
  elle revient sur le projecteur **au noir**, et la régie rallume avec Ctrl+B.
  Les enregistrements sont atomiques : un plantage pendant l'écriture ne corrompt pas le projet.
- **Projets** `.lanterne` (JSON lisible), chemins relatifs au projet pour déplacer un dossier de spectacle d'une machine à l'autre.
  Un fichier introuvable à l'ouverture n'est pas perdu : son chemin est conservé et signalé dans le chutier.
- **Glisser-déposer** de vidéos, images, shaders ISF (un filtre déposé s'ajoute comme effet au calque sélectionné).

Shaders fournis (dossier `isf/`) : Mire de calage, Plasma, Dégradé, Nuages, Couleur unie ;
effets Couleur, Teinte, Flou (2 passes), Rémanence (tampon persistant), Kaléidoscope, Bords doux, Masque, Orientation, Pixels.

## Compiler

Au premier `cmake`, les sources de **Syphon** (macOS) ou de **Spout** (Windows) sont téléchargées depuis GitHub
et compilées avec Lanterne : une connexion internet est nécessaire la première fois.
Pour s'en passer : `-DLANTERNE_SYPHON=OFF` ou `-DLANTERNE_SPOUT=OFF`.

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
| Nouveau calque | bouton **+** de la liste des calques, menu Calque, glisser un fichier (du système ou du chutier) |
| Importer dans le chutier | bouton Importer…, Ctrl+I (⌘I), ou déposer des fichiers / un dossier sur le chutier |
| Retrouver un fichier déplacé | chutier ▸ sélectionner le fichier en rouge ▸ Remplacer… |
| Choisir l'écran du vidéoprojecteur | onglet Master ▸ Sortie vidéo, ou menu Sortie ▸ Écran de sortie |
| Plein écran / retour | Ctrl+F (⌘F sur Mac) |
| Sortie dans une fenêtre | Ctrl+Maj+F (⌘⇧F sur Mac) |
| Noir en fondu / retour | Ctrl+B (⌘B), ou bouton Noir du panneau Master |
| Annuler / rétablir | Ctrl+Z / Ctrl+Maj+Z ou Ctrl+Y (⌘Z / ⌘⇧Z) |
| Fermer la sortie depuis la sortie | Maj+Échap (Échap seul ne fait rien, par sécurité) |
| Régler la composition sur le projecteur | onglet Master ▸ Composition ▸ « = écran de sortie » |
| Publier en NDI, OMT, Syphon, Spout | onglet Master ▸ Publication de la sortie |
| Lecture / pause du calque vidéo sélectionné | Espace |
| Déplacer une poignée | glisser (Maj : déplacement fin) |
| Déplacer tout le calque | glisser à l'intérieur du calque |
| Ajuster au pixel | flèches (Maj : 10 px) |
| Poignée suivante / désélection | Tab / Échap |
| Mode coins ↔ grille | onglet Calque ▸ Mapping |
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
  Engine        composition, fil de rendu, présentation de la sortie, projet JSON, médias du chutier
  Publish       publication : NDI / OMT (chargés à l'exécution), Syphon (.mm), Spout ; relecture GPU asynchrone
  Isf           parseur et rendu ISF (traduction GLSL 330 core, passes, tampons)
  VideoDecoder  décodage FFmpeg dans un thread, file d'images, boucle sans couture, positionnement
  Mapping       homographie 4 coins + grille Catmull-Rom
src/ui/       interface Qt Widgets
  MainWindow, LayerInspector, ParamPanel, MappingView (édition), OutputWindow (projecteur)
  Commands      commandes d'annulation (QUndoStack)
  MediaBin, LayerTable, MasterPanel   chutier, liste des calques, onglet Master
isf/          shaders fournis
test/         tests automatiques
```

Le moteur possède son contexte OpenGL dans un fil dédié : il rend la composition, la présente dans la fenêtre
de sortie (synchro verticale) et publie une copie pour l'aperçu de l'interface.
L'interface lit et modifie les calques sous `Engine::Lock` ; tout ce qui touche OpenGL (compilation de shaders,
libération de textures) passe par `Engine::runGl()`, exécuté dans le fil de rendu.
On peut donc remplacer l'interface, ou piloter le moteur en OSC, sans toucher au rendu.

## Tests

```bash
test/make_media.sh                         # médias de test, une fois
cmake --build build --target lanterne_tests
./build/lanterne_tests                     # projet, vidéo, ISF, annulation, fil de rendu, master
test/ui_test.sh out/ui                     # Linux + Xvfb + openbox + xdotool : scénario complet de l'interface
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
- **Pas d'entrées Syphon / Spout / NDI / OMT, ni de caméra** (la sortie, elle, peut être publiée).
- NDI et OMT envoient l'image à la taille de la composition, relue depuis la carte graphique (une image de latence) ;
  pour une composition 4K, comptez une charge processeur notable, surtout pour OMT qui encode l'image.
- Sous Linux, OMT a besoin du service Avahi (`avahi-daemon`) pour la découverte ; Lanterne refuse de l'activer sans lui
  (la bibliothèque arrêterait le programme).
- Syphon est compilé en version OpenGL uniquement (pas de serveur Metal).
- Entrées audio des shaders ISF non gérées (texture noire).
- La lecture vidéo (lecture, pause, position) n'est pas annulable, volontairement ; la taille de la composition non plus.
- Annuler une modification de la chaîne d'effets recharge les effets du calque (les tampons de rémanence repartent de zéro).

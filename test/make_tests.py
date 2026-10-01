import json, os, glob
root = os.path.abspath(os.path.dirname(__file__))
isf = os.path.join(root, '..', 'isf')
media = os.path.join(root, 'media')
os.makedirs(os.path.join(root, 'projects'), exist_ok=True)

def layer(name, source, effects=(), mapping=None, blend='normal', opacity=1.0):
    l = {"name": name, "visible": True, "opacity": opacity, "blend": blend, "source": source,
         "effects": list(effects)}
    if mapping: l["mapping"] = mapping
    return l

def proj(name, layers, w=640, h=360):
    p = {"app": "Lanterne", "formatVersion": 1, "composition": {"width": w, "height": h}, "layers": layers}
    path = os.path.join(root, 'projects', name + '.lanterne')
    json.dump(p, open(path, 'w'), indent=1)
    return path

full = {"corners": [[0,0],[1,0],[1,1],[0,1]], "cols": 4, "rows": 4, "offsets": [[0,0]]*16}
video = lambda f: {"type": "video", "path": os.path.join(media, f), "loop": True, "speed": 1, "playing": True}

for g in sorted(glob.glob(os.path.join(isf, 'generateurs', '*.fs'))):
    n = os.path.splitext(os.path.basename(g))[0]
    proj('gen_' + n, [layer(n, {"type": "isf", "path": g, "width": 640, "height": 360}, mapping=full)])

for e in sorted(glob.glob(os.path.join(isf, 'effets', '*.fs'))):
    n = os.path.splitext(os.path.basename(e))[0]
    params = {}
    if n == 'Masque': params = {"maskImage": os.path.join(media, 'bars.png'), "feather": 0.1}
    if n == 'BordsDoux': params = {"left": 0.3, "right": 0.3, "top": 0.15, "bottom": 0.15}
    if n == 'Flou': params = {"radius": 20}
    if n == 'Teinte': params = {"hue": 120}
    if n == 'Pixels': params = {"cellSize": 24, "gap": 0.1}
    if n == 'Orientation': params = {"flipH": True}
    if n == 'Remanence': params = {"drift": [3, 0], "decay": 0.95}
    proj('fx_' + n, [layer('video', video('h264.mp4'), [{"path": e, "enabled": True, "params": params}], mapping=full)])

# Codecs vidéo
for f in ['h264.mp4', 'prores.mov', 'hap.mov']:
    proj('codec_' + f.split('.')[0], [layer(f, video(f), mapping=full)])

# Mapping : perspective + grille déformée, deux calques, fusion addition
mire = os.path.join(isf, 'generateurs', 'Mire.fs')
plasma = os.path.join(isf, 'generateurs', 'Plasma.fs')
offs = [[0,0]]*16
offs = [list(o) for o in offs]
offs[5] = [0.05, -0.06]; offs[6] = [-0.03, 0.08]; offs[9] = [0.04, 0.04]
warp = {"corners": [[0.08,0.05],[0.62,0.12],[0.58,0.95],[0.03,0.82]], "cols": 4, "rows": 4, "offsets": offs, "meshMode": True}
quad = {"corners": [[0.55,0.1],[0.97,0.02],[0.95,0.7],[0.6,0.6]], "cols": 4, "rows": 4, "offsets": [[0,0]]*16}
proj('mapping', [
    layer('Plasma', {"type": "isf", "path": plasma, "width": 512, "height": 512}, mapping=quad, blend='add', opacity=0.9),
    layer('Mire', {"type": "isf", "path": mire, "width": 1280, "height": 720}, mapping=warp),
])
print('ok')

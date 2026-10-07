import json, os, glob
root = os.path.abspath(os.path.dirname(__file__))
isf = os.path.join(root, '..', 'isf')
media = os.path.join(root, 'media')
os.makedirs(os.path.join(root, 'projects'), exist_ok=True)

def layer(name, source, effects=(), mapping=None, blend='normal', opacity=1.0):
    l = {"name": name, "enable": True, "opacity": opacity, "blend_mode": blend, "source": source,
         "effects": list(effects)}
    if mapping: l["spatial"] = mapping
    return l

def proj(name, layers, w=640, h=360):
    p = {"app": "Fulskrin", "format_version": 1, "composition": {"width": w, "height": h}, "layers": layers}
    path = os.path.join(root, 'projects', name + '.fulskrin')
    json.dump(p, open(path, 'w'), indent=1)
    return path

full = {"corners": [[0,0],[1,0],[1,1],[0,1]], "cols": 4, "rows": 4, "offsets": [[0,0]]*16}
video = lambda f: {"type": "video", "path": os.path.join(media, f), "loop": True, "speed": 1, "playing": True}

for g in sorted(glob.glob(os.path.join(isf, 'generators', '*.fs'))):
    n = os.path.splitext(os.path.basename(g))[0]
    proj('gen_' + n, [layer(n, {"type": "isf", "path": g, "width": 640, "height": 360}, mapping=full)])

for e in sorted(glob.glob(os.path.join(isf, 'effects', '*.fs'))):
    n = os.path.splitext(os.path.basename(e))[0]
    params = {}
    if n == 'Mask': params = {"maskImage": os.path.join(media, 'bars.png'), "feather": 0.1}
    if n == 'SoftEdges': params = {"left": 0.3, "right": 0.3, "top": 0.15, "bottom": 0.15}
    if n == 'Blur': params = {"radius": 20}
    if n == 'Hue': params = {"hue": 120}
    if n == 'Pixelate': params = {"cellSize": 24, "gap": 0.1}
    if n == 'FlipCrop': params = {"flipH": True}
    if n == 'Trails': params = {"drift": [3, 0], "decay": 0.95}
    proj('fx_' + n, [layer('video', video('h264.mp4'), [{"path": e, "enable": True, "params": params}], mapping=full)])

# Video codecs
for f in ['h264.mp4', 'prores.mov', 'hap.mov']:
    proj('codec_' + f.split('.')[0], [layer(f, video(f), mapping=full)])

# Mapping: perspective + warped grid, two layers, additive blend
pattern = os.path.join(isf, 'generators', 'TestPattern.fs')
plasma = os.path.join(isf, 'generators', 'Plasma.fs')
offs = [[0,0]]*16
offs = [list(o) for o in offs]
offs[5] = [0.05, -0.06]; offs[6] = [-0.03, 0.08]; offs[9] = [0.04, 0.04]
warp = {"corners": [[0.08,0.05],[0.62,0.12],[0.58,0.95],[0.03,0.82]], "cols": 4, "rows": 4, "offsets": offs, "mesh_mode": True}
quad = {"corners": [[0.55,0.1],[0.97,0.02],[0.95,0.7],[0.6,0.6]], "cols": 4, "rows": 4, "offsets": [[0,0]]*16}
proj('mapping', [
    layer('Plasma', {"type": "isf", "path": plasma, "width": 512, "height": 512}, mapping=quad, blend='add', opacity=0.9),
    layer('Test Pattern', {"type": "isf", "path": pattern, "width": 1280, "height": 720}, mapping=warp),
])
# UI demo (test/ui_test.sh)
video_quad = {"corners": [[0.0,0.45],[0.45,0.5],[0.42,1.0],[0.0,0.98]], "cols": 4, "rows": 4, "offsets": [[0,0]]*16}
demo = {"app": "Fulskrin", "format_version": 1, "composition": {"width": 640, "height": 360}, "layers": [
    layer('Plasma', {"type": "isf", "path": plasma, "width": 512, "height": 512}, mapping=quad, blend='add', opacity=0.9),
    layer('Test Pattern', {"type": "isf", "path": pattern, "width": 1280, "height": 720}, mapping=warp),
    layer('Video h264', video('h264.mp4'), [
        {"path": os.path.join(isf, 'effects', 'ColorCorrection.fs'), "enable": True, "params": {"saturation": 0.3}},
        {"path": os.path.join(isf, 'effects', 'SoftEdges.fs'), "enable": True, "params": {"left": 0.2, "right": 0.2}}], mapping=video_quad),
    layer('Act 2', {"type": "video", "path": os.path.join(root, 'out', 'missing', 'act2.mov'), "loop": True, "speed": 1, "playing": True}),
], "bin": [{"path": os.path.join(media, 'bars.png')}], "ui": {"selected_layer": 1}}
json.dump(demo, open(os.path.join(root, 'projects', 'demo.fulskrin'), 'w'), indent=1)
print('ok')

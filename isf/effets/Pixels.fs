/*{
    "DESCRIPTION": "Pixelisation (mosaïque) avec cellules carrées.",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Stylisation"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "cellSize", "LABEL": "Taille (px)", "TYPE": "float", "DEFAULT": 16.0, "MIN": 1.0, "MAX": 200.0 },
        { "NAME": "gap", "LABEL": "Interstice", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.45 }
    ]
}*/

void main()
{
    vec2 px = isf_FragNormCoord * RENDERSIZE;
    vec2 cell = floor(px / cellSize);
    vec2 centerPx = (cell + 0.5) * cellSize;
    vec4 c = IMG_PIXEL(inputImage, centerPx);
    vec2 f = fract(px / cellSize);
    float inside = step(gap, f.x) * step(gap, f.y) * step(f.x, 1.0 - gap) * step(f.y, 1.0 - gap);
    gl_FragColor = vec4(c.rgb * inside, c.a);
}

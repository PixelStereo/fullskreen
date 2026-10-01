/*{
    "DESCRIPTION": "Miroir horizontal / vertical (rétroprojection, projecteur au plafond) et recadrage.",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Géométrie"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "flipH", "LABEL": "Miroir horizontal", "TYPE": "bool", "DEFAULT": false },
        { "NAME": "flipV", "LABEL": "Miroir vertical", "TYPE": "bool", "DEFAULT": false },
        { "NAME": "cropMin", "LABEL": "Recadrage début", "TYPE": "point2D", "DEFAULT": [0.0, 0.0], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] },
        { "NAME": "cropMax", "LABEL": "Recadrage fin", "TYPE": "point2D", "DEFAULT": [1.0, 1.0], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] }
    ]
}*/

void main()
{
    vec2 uv = isf_FragNormCoord;
    if (flipH) uv.x = 1.0 - uv.x;
    if (flipV) uv.y = 1.0 - uv.y;
    // le recadrage est exprimé avec l'origine en haut à gauche
    vec2 lo = vec2(cropMin.x, 1.0 - cropMax.y);
    vec2 hi = vec2(cropMax.x, 1.0 - cropMin.y);
    uv = mix(lo, hi, uv);
    gl_FragColor = IMG_NORM_PIXEL(inputImage, uv);
}

/*{
    "DESCRIPTION": "Horizontal / vertical flip (rear projection, ceiling-mounted projector) and crop.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Geometry"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "flipH", "LABEL": "Flip horizontal", "TYPE": "bool", "DEFAULT": false },
        { "NAME": "flipV", "LABEL": "Flip vertical", "TYPE": "bool", "DEFAULT": false },
        { "NAME": "cropMin", "LABEL": "Crop start", "TYPE": "point2D", "DEFAULT": [0.0, 0.0], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] },
        { "NAME": "cropMax", "LABEL": "Crop end", "TYPE": "point2D", "DEFAULT": [1.0, 1.0], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] }
    ]
}*/

void main()
{
    vec2 uv = isf_FragNormCoord;
    if (flipH) uv.x = 1.0 - uv.x;
    if (flipV) uv.y = 1.0 - uv.y;
    // the crop is expressed with the origin at the top left
    vec2 lo = vec2(cropMin.x, 1.0 - cropMax.y);
    vec2 hi = vec2(cropMax.x, 1.0 - cropMin.y);
    uv = mix(lo, hi, uv);
    gl_FragColor = IMG_NORM_PIXEL(inputImage, uv);
}

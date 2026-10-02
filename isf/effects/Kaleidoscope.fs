/*{
    "DESCRIPTION": "Rotating kaleidoscope with mirrored segments.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Geometry"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "segments", "LABEL": "Segments", "TYPE": "float", "DEFAULT": 6.0, "MIN": 1.0, "MAX": 24.0 },
        { "NAME": "rotation", "LABEL": "Rotation (°)", "TYPE": "float", "DEFAULT": 0.0, "MIN": -180.0, "MAX": 180.0 },
        { "NAME": "spin", "LABEL": "Auto rotation (turns/s)", "TYPE": "float", "DEFAULT": 0.0, "MIN": -1.0, "MAX": 1.0 },
        { "NAME": "zoom", "LABEL": "Zoom", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.2, "MAX": 4.0 },
        { "NAME": "center", "LABEL": "Center", "TYPE": "point2D", "DEFAULT": [0.5, 0.5], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] }
    ]
}*/

void main()
{
    float aspect = RENDERSIZE.x / RENDERSIZE.y;
    vec2 p = isf_FragNormCoord - center;
    p.x *= aspect;
    float r = length(p) / zoom;
    float a = atan(p.y, p.x) + radians(rotation) + TIME * spin * 6.2831853;
    float seg = 6.2831853 / floor(segments);
    a = mod(a, seg);
    a = abs(a - seg * 0.5);
    vec2 q = vec2(cos(a), sin(a)) * r;
    q.x /= aspect;
    gl_FragColor = IMG_NORM_PIXEL(inputImage, clamp(q + 0.5, 0.0, 1.0));
}

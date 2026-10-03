/*{
    "DESCRIPTION": "Iris: the incoming picture opens as a circle from a point.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Transition"],
    "INPUTS": [
        { "NAME": "startImage", "TYPE": "image" },
        { "NAME": "endImage", "TYPE": "image" },
        { "NAME": "progress", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0 },
        { "NAME": "center", "LABEL": "Center", "TYPE": "point2D", "DEFAULT": [0.5, 0.5], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] },
        { "NAME": "softness", "LABEL": "Softness", "TYPE": "float", "DEFAULT": 0.05, "MIN": 0.0, "MAX": 0.5 }
    ]
}*/

void main()
{
    vec2 uv = isf_FragNormCoord;
    vec2 aspect = vec2(RENDERSIZE.x / max(1.0, RENDERSIZE.y), 1.0);
    // Radius that covers the whole picture from the center
    vec2 far = max(center, 1.0 - center) * aspect;
    float maxR = length(far) + softness;
    float d = length((uv - center) * aspect);
    float r = progress * maxR;
    float k = 1.0 - smoothstep(r - softness, r + 1e-4, d);
    gl_FragColor = mix(IMG_NORM_PIXEL(startImage, uv), IMG_NORM_PIXEL(endImage, uv), k);
}

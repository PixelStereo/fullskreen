/*{
    "DESCRIPTION": "Wipe: a soft edge sweeps across the picture.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Transition"],
    "INPUTS": [
        { "NAME": "startImage", "TYPE": "image" },
        { "NAME": "endImage", "TYPE": "image" },
        { "NAME": "progress", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0 },
        { "NAME": "angle", "LABEL": "Angle", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 360.0 },
        { "NAME": "softness", "LABEL": "Softness", "TYPE": "float", "DEFAULT": 0.1, "MIN": 0.0, "MAX": 0.5 }
    ]
}*/

void main()
{
    vec2 uv = isf_FragNormCoord;
    float r = radians(angle);
    vec2 d = vec2(cos(r), sin(r));
    // Position along the direction, 0 at the first corner reached, 1 at the last
    float lo = min(0.0, d.x) + min(0.0, d.y), hi = max(0.0, d.x) + max(0.0, d.y);
    float x = (dot(uv, d) - lo) / max(1e-4, hi - lo);
    float edge = progress * (1.0 + softness) - softness;
    float k = 1.0 - smoothstep(edge, edge + softness + 1e-4, x);
    gl_FragColor = mix(IMG_NORM_PIXEL(startImage, uv), IMG_NORM_PIXEL(endImage, uv), k);
}

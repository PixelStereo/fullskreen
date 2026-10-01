/*{
    "DESCRIPTION": "Fondu des bords (soft edge) réglable côté par côté, pour le blending entre vidéoprojecteurs ou les découpes.",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Calage"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "left", "LABEL": "Gauche", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "right", "LABEL": "Droite", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "top", "LABEL": "Haut", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "bottom", "LABEL": "Bas", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "curve", "LABEL": "Courbe (gamma)", "TYPE": "float", "DEFAULT": 2.2, "MIN": 0.5, "MAX": 4.0 }
    ]
}*/

float ramp(float x, float w)
{
    return w <= 0.0 ? 1.0 : smoothstep(0.0, 1.0, clamp(x / w, 0.0, 1.0));
}

void main()
{
    vec2 uv = isf_FragNormCoord;
    float k = ramp(uv.x, left) * ramp(1.0 - uv.x, right) * ramp(1.0 - uv.y, top) * ramp(uv.y, bottom);
    k = pow(k, 1.0 / curve);
    vec4 c = IMG_THIS_PIXEL(inputImage);
    gl_FragColor = vec4(c.rgb * k, c.a);
}

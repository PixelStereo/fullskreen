/*{
    "DESCRIPTION": "Plasma animé à deux couleurs.",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Génératif"],
    "INPUTS": [
        { "NAME": "colorA", "LABEL": "Couleur A", "TYPE": "color", "DEFAULT": [1.0, 0.45, 0.1, 1.0] },
        { "NAME": "colorB", "LABEL": "Couleur B", "TYPE": "color", "DEFAULT": [0.1, 0.2, 0.8, 1.0] },
        { "NAME": "scale", "LABEL": "Échelle", "TYPE": "float", "DEFAULT": 3.0, "MIN": 0.2, "MAX": 20.0 },
        { "NAME": "speed", "LABEL": "Vitesse", "TYPE": "float", "DEFAULT": 0.5, "MIN": 0.0, "MAX": 4.0 },
        { "NAME": "contrast", "LABEL": "Contraste", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.2, "MAX": 4.0 }
    ]
}*/

void main()
{
    vec2 uv = isf_FragNormCoord * vec2(RENDERSIZE.x / RENDERSIZE.y, 1.0) * scale;
    float t = TIME * speed;
    float v = sin(uv.x + t);
    v += sin((uv.y + t) * 0.7);
    v += sin((uv.x + uv.y + t) * 0.6);
    vec2 c = uv + vec2(sin(t * 0.33), cos(t * 0.5)) * scale * 0.5;
    v += sin(sqrt(dot(c, c) + 1.0) + t);
    v = 0.5 + 0.5 * sin(v * 1.5);
    v = clamp((v - 0.5) * contrast + 0.5, 0.0, 1.0);
    gl_FragColor = mix(colorA, colorB, v);
}

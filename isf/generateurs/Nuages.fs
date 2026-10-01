/*{
    "DESCRIPTION": "Bruit fractal animé (nuages, fumée, textures organiques).",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Génératif"],
    "INPUTS": [
        { "NAME": "colorA", "LABEL": "Ombre", "TYPE": "color", "DEFAULT": [0.02, 0.03, 0.06, 1.0] },
        { "NAME": "colorB", "LABEL": "Lumière", "TYPE": "color", "DEFAULT": [0.85, 0.9, 1.0, 1.0] },
        { "NAME": "scale", "LABEL": "Échelle", "TYPE": "float", "DEFAULT": 3.0, "MIN": 0.5, "MAX": 16.0 },
        { "NAME": "speed", "LABEL": "Vitesse", "TYPE": "float", "DEFAULT": 0.15, "MIN": 0.0, "MAX": 2.0 },
        { "NAME": "octaves", "LABEL": "Détail", "TYPE": "long", "VALUES": [1, 2, 3, 4, 5, 6, 7], "DEFAULT": 5 },
        { "NAME": "contrast", "LABEL": "Contraste", "TYPE": "float", "DEFAULT": 1.4, "MIN": 0.2, "MAX": 4.0 },
        { "NAME": "direction", "LABEL": "Vent", "TYPE": "point2D", "DEFAULT": [1.0, 0.2], "MIN": [-1.0, -1.0], "MAX": [1.0, 1.0] }
    ]
}*/

float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void main()
{
    vec2 p = isf_FragNormCoord * vec2(RENDERSIZE.x / RENDERSIZE.y, 1.0) * scale;
    p += direction * TIME * speed;
    float v = 0.0, amp = 0.5;
    mat2 rot = mat2(0.8, -0.6, 0.6, 0.8);
    for (int i = 0; i < 7; ++i) {
        if (i >= octaves) break;
        v += amp * noise(p + TIME * speed * 0.3 * float(i));
        p = rot * p * 2.02;
        amp *= 0.5;
    }
    v = clamp((v - 0.5) * contrast + 0.5, 0.0, 1.0);
    gl_FragColor = mix(colorA, colorB, v);
}

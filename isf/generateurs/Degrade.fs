/*{
    "DESCRIPTION": "Dégradé linéaire ou radial, orientable et animable.",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Génératif"],
    "INPUTS": [
        { "NAME": "colorA", "LABEL": "Début", "TYPE": "color", "DEFAULT": [0.0, 0.0, 0.0, 1.0] },
        { "NAME": "colorB", "LABEL": "Fin", "TYPE": "color", "DEFAULT": [1.0, 1.0, 1.0, 1.0] },
        { "NAME": "shape", "LABEL": "Forme", "TYPE": "long", "VALUES": [0, 1], "LABELS": ["Linéaire", "Radial"], "DEFAULT": 0 },
        { "NAME": "angle", "LABEL": "Angle (°)", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 360.0 },
        { "NAME": "center", "LABEL": "Centre", "TYPE": "point2D", "DEFAULT": [0.5, 0.5], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] },
        { "NAME": "repeat", "LABEL": "Répétitions", "TYPE": "float", "DEFAULT": 1.0, "MIN": 1.0, "MAX": 20.0 },
        { "NAME": "speed", "LABEL": "Défilement", "TYPE": "float", "DEFAULT": 0.0, "MIN": -2.0, "MAX": 2.0 }
    ]
}*/

void main()
{
    vec2 p = isf_FragNormCoord - center;
    p.x *= RENDERSIZE.x / RENDERSIZE.y;
    float t;
    if (shape == 0) {
        float a = radians(angle);
        t = dot(p, vec2(cos(a), sin(a))) + 0.5;
    } else {
        t = length(p) * 2.0;
    }
    t = t * repeat + TIME * speed;
    t = abs(fract(t * 0.5) * 2.0 - 1.0); // aller-retour pour éviter les ruptures
    gl_FragColor = mix(colorA, colorB, smoothstep(0.0, 1.0, t));
}

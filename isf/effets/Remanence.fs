/*{
    "DESCRIPTION": "Traînée / rémanence : accumule les images précédentes dans un tampon persistant en virgule flottante.",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Temps"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "decay", "LABEL": "Persistance", "TYPE": "float", "DEFAULT": 0.9, "MIN": 0.0, "MAX": 0.995 },
        { "NAME": "mode", "LABEL": "Mode", "TYPE": "long", "VALUES": [0, 1], "LABELS": ["Maximum", "Fondu"], "DEFAULT": 0 },
        { "NAME": "drift", "LABEL": "Dérive", "TYPE": "point2D", "DEFAULT": [0.0, 0.0], "MIN": [-4.0, -4.0], "MAX": [4.0, 4.0] },
        { "NAME": "clear", "LABEL": "Effacer", "TYPE": "event" }
    ],
    "PASSES": [
        { "TARGET": "trail", "PERSISTENT": true, "FLOAT": true },
        { }
    ]
}*/

void main()
{
    if (PASSINDEX == 0) {
        vec4 cur = IMG_THIS_PIXEL(inputImage);
        vec4 prev = IMG_NORM_PIXEL(trail, isf_FragNormCoord - drift / RENDERSIZE);
        if (clear || FRAMEINDEX == 0) prev = vec4(0.0);
        prev *= decay;
        gl_FragColor = mode == 0 ? max(cur, prev) : mix(cur, prev, decay);
    } else {
        gl_FragColor = IMG_THIS_PIXEL(trail);
    }
}

/*{
    "DESCRIPTION": "Crossfade: the outgoing picture dissolves into the incoming one.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Transition"],
    "INPUTS": [
        { "NAME": "startImage", "TYPE": "image" },
        { "NAME": "endImage", "TYPE": "image" },
        { "NAME": "progress", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0 }
    ]
}*/

void main()
{
    vec4 a = IMG_NORM_PIXEL(startImage, isf_FragNormCoord);
    vec4 b = IMG_NORM_PIXEL(endImage, isf_FragNormCoord);
    gl_FragColor = mix(a, b, progress);
}

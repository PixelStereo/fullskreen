/*{
    "DESCRIPTION": "Dissolve: the incoming picture appears in a grain of noise.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Transition"],
    "INPUTS": [
        { "NAME": "startImage", "TYPE": "image" },
        { "NAME": "endImage", "TYPE": "image" },
        { "NAME": "progress", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0 },
        { "NAME": "grain", "LABEL": "Grain", "TYPE": "float", "DEFAULT": 120.0, "MIN": 2.0, "MAX": 800.0 },
        { "NAME": "softness", "LABEL": "Softness", "TYPE": "float", "DEFAULT": 0.1, "MIN": 0.0, "MAX": 0.5 }
    ]
}*/

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

float noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void main()
{
    vec2 uv = isf_FragNormCoord;
    vec2 aspect = vec2(RENDERSIZE.x / max(1.0, RENDERSIZE.y), 1.0);
    float n = noise(uv * aspect * grain / 10.0) * 0.6 + noise(uv * aspect * grain / 3.0) * 0.4;
    float edge = progress * (1.0 + softness) - softness;
    float k = 1.0 - smoothstep(edge, edge + softness + 1e-4, n);
    gl_FragColor = mix(IMG_NORM_PIXEL(startImage, uv), IMG_NORM_PIXEL(endImage, uv), k);
}

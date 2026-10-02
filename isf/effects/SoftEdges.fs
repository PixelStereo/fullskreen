/*{
    "DESCRIPTION": "Soft edge, adjustable per side, for blending between projectors or for cut-outs.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Alignment"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "left", "LABEL": "Left", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "right", "LABEL": "Right", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "top", "LABEL": "Top", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "bottom", "LABEL": "Bottom", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 },
        { "NAME": "curve", "LABEL": "Curve (gamma)", "TYPE": "float", "DEFAULT": 2.2, "MIN": 0.5, "MAX": 4.0 }
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

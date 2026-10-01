/*{
    "DESCRIPTION": "Flou gaussien séparable en deux passes (passe horizontale à demi-résolution, puis verticale).",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Flou"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "radius", "LABEL": "Rayon (px)", "TYPE": "float", "DEFAULT": 8.0, "MIN": 0.0, "MAX": 64.0 }
    ],
    "PASSES": [
        { "TARGET": "horizontal", "WIDTH": "floor($WIDTH / 2.0)", "HEIGHT": "floor($HEIGHT / 2.0)" },
        { }
    ]
}*/

const int TAPS = 12;

vec4 blur(vec2 dir, vec2 uvCenter)
{
    float sigma = max(radius, 0.001) / 2.0;
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int i = -TAPS; i <= TAPS; ++i) {
        float x = float(i) / float(TAPS) * radius;
        float w = exp(-0.5 * x * x / (sigma * sigma));
        vec2 uv = uvCenter + dir * x / RENDERSIZE;
        vec4 s = PASSINDEX == 0 ? IMG_NORM_PIXEL(inputImage, uv) : IMG_NORM_PIXEL(horizontal, uv);
        acc += s * w;
        wsum += w;
    }
    return acc / wsum;
}

void main()
{
    if (radius < 0.01) {
        gl_FragColor = IMG_THIS_PIXEL(inputImage);
        return;
    }
    if (PASSINDEX == 0)
        gl_FragColor = blur(vec2(1.0, 0.0) * 0.5, isf_FragNormCoord);
    else
        gl_FragColor = blur(vec2(0.0, 1.0), isf_FragNormCoord);
}

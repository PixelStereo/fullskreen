/*{
    "DESCRIPTION": "Mask from an image (luminance or alpha): shape cut-out for mapping.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Alignment"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "maskImage", "LABEL": "Mask image", "TYPE": "image" },
        { "NAME": "source", "LABEL": "Channel", "TYPE": "long", "VALUES": [0, 1], "LABELS": ["Luminance", "Alpha"], "DEFAULT": 0 },
        { "NAME": "invert", "LABEL": "Invert", "TYPE": "bool", "DEFAULT": false },
        { "NAME": "feather", "LABEL": "Feather", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 0.5 }
    ]
}*/

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    vec4 m = IMG_NORM_PIXEL(maskImage, isf_FragNormCoord);
    float k = source == 0 ? dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) : m.a;
    if (IMG_SIZE(maskImage).x <= 1.0) k = 1.0; // no image: no mask
    if (invert) k = 1.0 - k;
    k = smoothstep(0.5 - feather - 0.001, 0.5 + feather + 0.001, k);
    gl_FragColor = vec4(c.rgb, c.a * k);
}

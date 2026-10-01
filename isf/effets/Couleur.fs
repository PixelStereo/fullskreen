/*{
    "DESCRIPTION": "Correction colorimétrique : exposition, contraste, saturation, gamma, teinte RVB.",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Couleur"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "exposure", "LABEL": "Exposition (IL)", "TYPE": "float", "DEFAULT": 0.0, "MIN": -4.0, "MAX": 4.0 },
        { "NAME": "contrast", "LABEL": "Contraste", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.0, "MAX": 3.0 },
        { "NAME": "saturation", "LABEL": "Saturation", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.0, "MAX": 3.0 },
        { "NAME": "gamma", "LABEL": "Gamma", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.2, "MAX": 3.0 },
        { "NAME": "tint", "LABEL": "Multiplier par", "TYPE": "color", "DEFAULT": [1.0, 1.0, 1.0, 1.0] }
    ]
}*/

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    vec3 rgb = c.rgb * exp2(exposure);
    rgb = (rgb - 0.5) * contrast + 0.5;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luma), rgb, saturation);
    rgb = pow(max(rgb, 0.0), vec3(1.0 / gamma));
    rgb *= tint.rgb;
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), c.a * tint.a);
}

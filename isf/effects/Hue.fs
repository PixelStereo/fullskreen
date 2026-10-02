/*{
    "DESCRIPTION": "Hue rotation, with optional animation.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Color"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "hue", "LABEL": "Hue (°)", "TYPE": "float", "DEFAULT": 0.0, "MIN": -180.0, "MAX": 180.0 },
        { "NAME": "cycle", "LABEL": "Cycle (turns/s)", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 2.0 }
    ]
}*/

vec3 rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    vec3 hsv = rgb2hsv(c.rgb);
    hsv.x = fract(hsv.x + hue / 360.0 + TIME * cycle);
    gl_FragColor = vec4(hsv2rgb(hsv), c.a);
}

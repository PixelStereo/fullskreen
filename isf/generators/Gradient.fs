/*{
    "DESCRIPTION": "Linear or radial gradient, rotatable and animatable.",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Generative"],
    "INPUTS": [
        { "NAME": "colorA", "LABEL": "Start", "TYPE": "color", "DEFAULT": [0.0, 0.0, 0.0, 1.0] },
        { "NAME": "colorB", "LABEL": "End", "TYPE": "color", "DEFAULT": [1.0, 1.0, 1.0, 1.0] },
        { "NAME": "shape", "LABEL": "Shape", "TYPE": "long", "VALUES": [0, 1], "LABELS": ["Linear", "Radial"], "DEFAULT": 0 },
        { "NAME": "angle", "LABEL": "Angle (°)", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 360.0 },
        { "NAME": "center", "LABEL": "Center", "TYPE": "point2D", "DEFAULT": [0.5, 0.5], "MIN": [0.0, 0.0], "MAX": [1.0, 1.0] },
        { "NAME": "repeat", "LABEL": "Repeats", "TYPE": "float", "DEFAULT": 1.0, "MIN": 1.0, "MAX": 20.0 },
        { "NAME": "speed", "LABEL": "Scroll", "TYPE": "float", "DEFAULT": 0.0, "MIN": -2.0, "MAX": 2.0 }
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
    t = abs(fract(t * 0.5) * 2.0 - 1.0); // ping-pong to avoid discontinuities
    gl_FragColor = mix(colorA, colorB, smoothstep(0.0, 1.0, t));
}

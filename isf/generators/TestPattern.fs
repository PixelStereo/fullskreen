/*{
    "DESCRIPTION": "Alignment test pattern for mapping: grid, center cross, circles, diagonals, border and colored corners (orientation).",
    "CREDIT": "Fulskrin",
    "ISFVSN": "2",
    "CATEGORIES": ["Alignment"],
    "INPUTS": [
        { "NAME": "divisions", "LABEL": "Divisions", "TYPE": "float", "DEFAULT": 12, "MIN": 2, "MAX": 64 },
        { "NAME": "lineWidth", "LABEL": "Line width (px)", "TYPE": "float", "DEFAULT": 2, "MIN": 1, "MAX": 12 },
        { "NAME": "lineColor", "LABEL": "Line color", "TYPE": "color", "DEFAULT": [1.0, 1.0, 1.0, 1.0] },
        { "NAME": "background", "LABEL": "Background", "TYPE": "color", "DEFAULT": [0.04, 0.04, 0.05, 1.0] },
        { "NAME": "showCircles", "LABEL": "Circles", "TYPE": "bool", "DEFAULT": true },
        { "NAME": "showDiagonals", "LABEL": "Diagonals", "TYPE": "bool", "DEFAULT": true },
        { "NAME": "colorCorners", "LABEL": "Colored corners", "TYPE": "bool", "DEFAULT": true }
    ]
}*/

float lineMask(float d, float w)
{
    return 1.0 - smoothstep(w * 0.5 - 0.75, w * 0.5 + 0.75, d);
}

void main()
{
    vec2 px = isf_FragNormCoord * RENDERSIZE;
    vec2 center = RENDERSIZE * 0.5;
    float cell = RENDERSIZE.y / floor(divisions);

    // Grid aligned on the center
    vec2 g = abs(mod(px - center + cell * 0.5, cell) - cell * 0.5);
    float grid = lineMask(min(g.x, g.y), lineWidth);

    // Thick center cross
    vec2 dc = abs(px - center);
    float cross = lineMask(min(dc.x, dc.y), lineWidth * 2.5);

    // Image border
    vec2 de = min(px, RENDERSIZE - px);
    float border = lineMask(min(de.x, de.y), lineWidth * 4.0);

    float circles = 0.0;
    if (showCircles) {
        float r = length(px - center);
        float R = RENDERSIZE.y * 0.5 - lineWidth * 3.0;
        circles = lineMask(abs(r - R), lineWidth * 1.5) + lineMask(abs(r - R * 0.5), lineWidth);
        float cr = RENDERSIZE.y * 0.11;
        circles += lineMask(abs(length(de - vec2(cr * 1.3)) - cr), lineWidth);
    }

    float diag = 0.0;
    if (showDiagonals) {
        vec2 a = RENDERSIZE;
        float len = length(a);
        float d1 = abs(px.x * a.y - px.y * a.x) / len;
        float d2 = abs(px.x * a.y + px.y * a.x - a.x * a.y) / len;
        diag = lineMask(min(d1, d2), lineWidth);
    }

    vec4 col = background;
    if (colorCorners) {
        vec2 uv = isf_FragNormCoord;
        // top-left red, top-right green, bottom-left blue, bottom-right yellow
        vec3 tint = uv.x < 0.5 ? (uv.y > 0.5 ? vec3(0.85, 0.12, 0.12) : vec3(0.12, 0.3, 0.9))
                               : (uv.y > 0.5 ? vec3(0.12, 0.75, 0.25) : vec3(0.9, 0.78, 0.1));
        vec2 q = min(uv, 1.0 - uv) * vec2(RENDERSIZE.x / RENDERSIZE.y, 1.0);
        float k = 1.0 - smoothstep(0.0, 0.32, length(q));
        col.rgb = mix(col.rgb, tint, k * 0.75);
    }

    float m = clamp(grid * 0.5 + cross + border + circles + diag * 0.6, 0.0, 1.0);
    gl_FragColor = mix(col, lineColor, m);
}

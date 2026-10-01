/*{
    "DESCRIPTION": "Test : shader avec vertex shader personnalisé (.vs) et varyings.",
    "ISFVSN": "2",
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "amount", "TYPE": "float", "DEFAULT": 20.0, "MIN": 0.0, "MAX": 100.0 }
    ]
}*/
varying vec2 leftCoord;
varying vec2 rightCoord;
void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    float r = IMG_NORM_PIXEL(inputImage, leftCoord).r;
    float b = IMG_NORM_PIXEL(inputImage, rightCoord).b;
    gl_FragColor = vec4(r, c.g, b, c.a);
}

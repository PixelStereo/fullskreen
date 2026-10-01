varying vec2 leftCoord;
varying vec2 rightCoord;
void main()
{
    isf_vertShaderInit();
    vec2 off = vec2(amount / RENDERSIZE.x, 0.0);
    leftCoord = isf_FragNormCoord - off;
    rightCoord = isf_FragNormCoord + off;
}

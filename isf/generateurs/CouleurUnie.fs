/*{
    "DESCRIPTION": "Aplat de couleur (fond, masque lumineux, test de projecteur).",
    "CREDIT": "Lanterne",
    "ISFVSN": "2",
    "CATEGORIES": ["Calage"],
    "INPUTS": [
        { "NAME": "color", "LABEL": "Couleur", "TYPE": "color", "DEFAULT": [1.0, 1.0, 1.0, 1.0] }
    ]
}*/

void main()
{
    gl_FragColor = color;
}

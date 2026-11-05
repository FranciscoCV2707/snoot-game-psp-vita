# Procedencia del port de PS Vita

Este proyecto genera un port nativo para PS Vita a partir del código fuente original de Snoot Game para PC.
No convierte los recursos de los ports de PS1 o PSP. El parser y el compilador VNB se adaptaron
para alimentar el intérprete nativo incluido aquí.

Las imágenes, el audio y el menú se generan desde los archivos originales. Vita usa resolución de 960x544.
Español es el único idioma y `persistent.lewd` se compila como verdadero.

`manifest.json` conserva los hashes de los guiones usados para generar los datos. Los recursos originales no
se incluyen en este repositorio: cada usuario debe generarlos desde una copia legal del juego para PC.

El juego y los recursos originales conservan las licencias y créditos de Cavemanon incluidos en su repositorio.

# CheatsPluginU

Plugin de Aroma para Wii U que carga archivos de trucos por Title ID desde la tarjeta SD y permite activar o desactivar cada archivo desde la configuración de WUPS.

## Descargar e instalar

1. Usa una Wii U con Aroma instalado.
2. Descarga el artefacto CheatsPlugin-wps de GitHub Actions o el archivo de una versión publicada.
3. Copia CheatsPlugin.wps a sd:/wiiu/environments/aroma/plugins/CheatsPlugin.wps.
4. Crea una carpeta para el juego en sd:/wiiu/cheatsPlugin/TITLE_ID/, usando el Title ID real de 16 caracteres hexadecimales en mayúsculas.
5. Guarda cada conjunto de trucos en un archivo .txt o .cht dentro de esa carpeta y abre la configuración del plugin cuando el juego correspondiente esté activo.

Ejemplo de estructura:

    sd:/wiiu/
    ├── environments/aroma/plugins/CheatsPlugin.wps
    └── cheatsPlugin/
        └── 000500001234ABCD/
            ├── velocidad.txt
            └── monedas.cht

Los nombres y Title ID del ejemplo son marcadores de posición. Usa el Title ID exacto del juego instalado y códigos verificados para su versión.

## Formato compatible

Esta versión no interpreta códigos Gecko, Action Replay ni formatos de otros gestores de trucos. Cada línea válida debe ser una escritura de 32 bits, con este formato:

    0xDIRECCION = 0xVALOR

Los valores anteriores son marcadores, no un truco funcional. Se permiten líneas vacías y comentarios que empiezan con # o ;. Las direcciones deben estar alineadas a 4 bytes y dentro del rango aceptado por el plugin.

## Activar y desactivar archivos

- Cada archivo válido aparece como interruptor individual en la configuración de CheatsPluginU cuando el juego correspondiente está activo.
- El estado de cada archivo se guarda de forma independiente por juego y nombre de archivo.
- Desactivar un archivo detiene la reaplicación de sus escrituras. Los cambios de memoria que ya se hayan aplicado no necesariamente se revierten.
- El interruptor global para habilitar o deshabilitar el plugin sigue disponible. Los cambios de ese interruptor global se aplican en el siguiente inicio del juego.

## Límites y seguridad

- Usa únicamente direcciones y valores verificados para el juego, Title ID y versión exactos. Un código incorrecto puede congelar el juego o alterar datos de la partida.
- Haz una copia de seguridad de los datos guardados y prueba primero en modo local.
- Solo se admiten escrituras directas de 32 bits. No se admiten códigos condicionales, códigos multilínea con sintaxis propia ni descifrado de códigos.
- El plugin requiere Aroma y los módulos de runtime que utiliza, incluidos KernelModule y NotificationModule.
- Que GitHub Actions compile el archivo confirma que se generó el binario; no demuestra que cada código funcione en una Wii U real. La prueba final debe hacerse en la consola.

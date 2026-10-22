# CheatsPluginU

Plugin de Aroma para Wii U que carga archivos de trucos por Title ID.

## Descargar

El archivo compilado `CheatsPlugin.wps` se guarda en la raíz de este repositorio. Cada compilación de GitHub Actions lo genera y actualiza automáticamente.

## Instalación

1. Usa una Wii U con Aroma instalado.
2. Copia `CheatsPlugin.wps` a `sd:/wiiu/environments/aroma/plugins/CheatsPlugin.wps`.
3. Crea una carpeta para el juego en `sd:/wiiu/cheatsPlugin/TITLE_ID/`, con su Title ID de 16 caracteres hexadecimales en mayúsculas.
4. Guarda tus archivos `.txt` o `.cht` en esa carpeta y reinicia el juego.

Ejemplo de estructura:

```text
sd:/wiiu/
├── environments/aroma/plugins/CheatsPlugin.wps
└── cheatsPlugin/
    └── 000500001234ABCD/
        └── ejemplo.txt
```

## Formato de archivo

Esta versión **no interpreta códigos Gecko, Action Replay ni formatos de otros gestores de trucos**. Cada línea válida debe ser una escritura de 32 bits en el formato:

```text
0xDIRECCION = 0xVALOR
```

Por ejemplo, los números de arriba son marcadores de posición, no un truco válido. Usa únicamente direcciones y valores verificados para el juego y la versión exactos. Las direcciones deben estar alineadas a 4 bytes y dentro del rango que acepta el plugin.

Todos los archivos válidos dentro de la carpeta del juego se aplican automáticamente; esta versión todavía no incluye una lista para activar o desactivar trucos individualmente. Un código incorrecto puede bloquear el juego o provocar pérdida de progreso. Prueba primero con datos guardados respaldados.

## Dependencias y estado

Necesita Aroma y los módulos de runtime que usa el plugin (incluidos KernelModule y NotificationModule). La compilación correcta solo confirma que se generó el binario; el funcionamiento real debe probarse en la consola.

El código fuente y el workflow de compilación están disponibles en este repositorio.

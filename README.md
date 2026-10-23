# CheatsPluginU

Plugin de Aroma para Wii U que carga archivos locales y busca automáticamente trucos online usando el Title ID del juego.

## Permisos de Aroma

La versión 0.5.0 solicita explícitamente `WUPS_FS_ACCESS()` para leer la SD y `WUPS_ALLOW_KERNEL()` para solicitar permisos de escritura en memoria. Al iniciar cada juego, comprueba los indicadores `sd_mounted` y `kernel_access`; si no están disponibles, muestra una notificación y no intenta escribir memoria.

## Búsqueda global por Wi-Fi

- En cada inicio de juego, consulta por HTTPS el listado actual de carpetas de juegos del repositorio público uCodes en GitHub.
- Recorre las carpetas numéricas descubiertas y descarga cada `info.json` hasta encontrar coincidencia exacta con el Title ID de 64 bits del juego abierto. Ya no se queda limitado a los índices `0–3`.
- Una vez localizado el juego, descarga su `codes.json` y muestra como interruptores solo los trucos que puede interpretar.
- La búsqueda depende de que el Wii U tenga conexión Wi-Fi/Internet y de que GitHub, uCodes y `CURLWrapperModule` estén accesibles.
- La cobertura no equivale a todos los juegos de Wii U: solo encuentra juegos que estén publicados en el catálogo uCodes. La inspección del repositorio upstream para esta versión encontró cuatro carpetas de juegos; el descubrimiento dinámico permite detectar nuevas carpetas que se añadan después.
- La búsqueda usa la API pública de GitHub sin autenticación, por lo que está sujeta a sus límites de solicitudes. En catálogos grandes, el recorrido secuencial también puede tardar.
- Los trucos online quedan desactivados por defecto para evitar aplicar cambios de memoria sin seleccionarlos. Los archivos locales también deben contener asignaciones directas de dirección/valor; no se ejecutan bloques de código-handler Gecko/Cafe.
- La versión actual solo convierte escrituras directas y constantes de formato `gecko.poke32(address, value)`. Los códigos con punteros, condiciones, secuencias u otros tipos de comandos se descartan en vez de aplicarse parcialmente.
- Si no existe una entrada con el Title ID exacto o no hay líneas compatibles, el plugin lo informa en una notificación.

## Requisitos

1. Wii U con Aroma y conexión a Internet.
2. `CheatsPlugin.wps` en `sd:/wiiu/environments/aroma/plugins/`.
3. `CURLWrapperModule.wms` instalado en `sd:/wiiu/environments/aroma/modules/` para las solicitudes HTTPS. Descárgalo del repositorio oficial: https://github.com/wiiu-env/CURLWrapperModule.

El plugin se enlaza con `libcurlwrapper`, pero el archivo `CURLWrapperModule.wms` debe instalarse por separado en la SD.

## Trucos locales

Los archivos locales van en `sd:/wiiu/cheatsPlugin/TITLE_ID/`. Cada archivo `.txt` o `.cht` debe contener escrituras directas de 32 bits, una por línea. Admite hexadecimal con o sin prefijo `0x` y los separadores `=` o `:`:

    0xDIRECCION = 0xVALOR
    10517B8C = 80800000

## Fuente y límites

La búsqueda utiliza la API pública de GitHub para enumerar carpetas y el contenido de https://github.com/vincent-coding/uCodes. Solo activa una coincidencia exacta de Title ID; no inventa códigos ni elige trucos por nombre aproximado.

## Precauciones

- Activa uno o pocos trucos a la vez, ya que un valor incorrecto puede congelar el juego o cambiar datos de partida.
- Desactivar un truco evita que se siga reaplicando, pero no restaura los valores que ya se escribieron.
- La compilación de GitHub Actions no sustituye una prueba en una Wii U real.

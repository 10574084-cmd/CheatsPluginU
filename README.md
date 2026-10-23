# CheatsPluginU

Plugin de Aroma para Wii U que carga archivos locales y busca automáticamente trucos online usando el Title ID del juego.

## Búsqueda automática

- En el inicio de cada juego, consulta la base de datos pública uCodes y comprueba el Title ID exacto.
- Los trucos compatibles aparecen como interruptores individuales en el menú de configuración de CheatsPlugin.
- Los trucos online quedan desactivados por defecto para evitar aplicar varios cambios desconocidos a la vez.
- La versión actual solo convierte escrituras directas y constantes de formato gecko.poke32(address, value). Los códigos con punteros, condiciones, secuencias u otros tipos de comandos se descartan en vez de aplicarse parcialmente.
- La búsqueda no inventa códigos: si no existe una entrada para ese Title ID o no hay líneas compatibles, lo informa con una notificación.

## Requisitos

1. Wii U con Aroma y conexión a Internet.
2. CheatsPlugin.wps en sd:/wiiu/environments/aroma/plugins/.
3. CURLWrapperModule.wms instalado en sd:/wiiu/environments/aroma/modules/ para las solicitudes HTTPS. Descárgalo del repositorio oficial: https://github.com/wiiu-env/CURLWrapperModule.

El plugin se enlaza con libcurlwrapper, pero el archivo CURLWrapperModule.wms debe instalarse por separado en la SD.

## Trucos locales

Los archivos locales van en sd:/wiiu/cheatsPlugin/TITLE_ID/. Cada archivo .txt o .cht debe contener escrituras directas de 32 bits con el formato:

    0xDIRECCION = 0xVALOR

## Fuente y cobertura

La búsqueda utiliza los metadatos y códigos publicados en https://github.com/vincent-coding/uCodes. El catálogo actual se consulta mediante sus entradas indexadas 0 a 3; por ello la cobertura se limita a los juegos para los que esa base contiene un Title ID. La base observada incluye una entrada para Mario Kart 8, pero su selección de códigos puede variar.

## Precauciones

- Activa uno o pocos trucos a la vez, ya que un valor incorrecto puede congelar el juego o cambiar datos de partida.
- Desactivar un truco evita que se siga reaplicando, pero no restaura los valores que ya se escribieron.
- La compilación de GitHub Actions no sustituye una prueba en una Wii U real.

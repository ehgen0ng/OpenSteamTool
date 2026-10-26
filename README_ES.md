<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="Logo de OpenSteamTool">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Herramienta de código abierto para desbloquear Steam</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-x64-2ea44f?logo=windows&logoColor=white" alt="Windows x64">
    <img src="https://img.shields.io/badge/macOS-arm64-2ea44f?logo=apple&logoColor=white" alt="macOS arm64">
    <a href="https://deepwiki.com/OpenSteam001/OpenSteamTool">
      <img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki">
    </a>
  </p>

  <p>
    <a href="README.md">
      <img src="https://flagcdn.com/w40/us.png" width="22" alt="Bandera de Estados Unidos">
      English
    </a>
    &nbsp;|&nbsp;
    <a href="README_ES.md">
      <img src="https://flagcdn.com/w40/es.png" width="22" alt="Bandera de España">
      Español
    </a>
  </p>
</div>

## Características

### Desbloqueos principales

- Desbloquea una cantidad ilimitada de juegos que no poseas.
- Desbloquea todos los DLC para juegos que no poseas.
- Soporta la carga automática de claves de descifrado de depósitos(depots) desde la configuración de Lua.
- Descarga manifiestos archivados de `manifest.luastools.xyz` a `depotcache` de Steam; si falla, continúa con el flujo existente de códigos de solicitud.
- Obtiene códigos de solicitud de manifiestos mediante las APIs de `opensteamtool` / `steamrun` / `wudrm` (por defecto, `opensteamtool`) o un endpoint personalizado de Lua (ver [Manifiestos y códigos de solicitud](#manifiestos-y-códigos-de-solicitud)).
- Soporta la descarga de juegos protegidos o DLCs que requieran un token de acceso.
- Soporta la vinculación de manifiestos para evitar que juegos específicos se actualicen.

### Recarga rápida

- Añadir, modificar, eliminar o sobrescribir archivos `.lua` en cualquier directorio supervisado activa automáticamente una recarga. No se necesita reiniciar ni alternar entre modo desconectado/conectado.

### Inyección

- Windows admite la inyección opcional de bibliotecas en procesos de juego mediante `[inject]` en `opensteamtool.toml`; esta función no está implementada en macOS.
- Configura `enabled`, `library_x64` y `library_x86`; la biblioteca debe coincidir con la arquitectura del proceso de destino. Las rutas pueden ser absolutas o relativas al directorio de datos.

### Préstamo familiar y juego en remoto

- Omite las restricciones de Steam Family Sharing para los juegos que se hayan añadido a la biblioteca con `addappid` en Lua. Todas las cuentas de la familia de Steam que participen en el préstamo familiar deben usar OpenSteamTool para que esto funcione.

### Compatible con juegos protegidos por Denuvo y SteamStub

- La detección y autorización de Denuvo solo se ejecutan en Windows. macOS incluye el almacenamiento y manejo compartido de tickets, pero no implementa este flujo de detección y autorización.
- Los juegos protegidos únicamente por SteamStub no requieren la configuración de `AppTicket`. OpenSteamTool puede reutilizar el ticket local de ConfigStore de Steam y falsificar el AppId solicitado a través de una vulnerabilidad de desbordamiento por cuatro (off-by-four ticket parsing vulnerability) en SteamDRMP, sin necesidad de inyectarse en el proceso del juego.
- Los juegos protegidos por Denuvo siguen requiriendo datos explícitos del ticket. OpenSteamTool almacena `AppTicket` y `ETicket` mediante el almacén de credenciales de la plataforma.
- Utiliza `setAppTicket(appid, "hex")` y `setETicket(appid, "hex")` en la configuración de Lua para escribir estos valores en el almacén de credenciales de forma automática.
- La verificación de Denuvo tiene una ventana de validez de 30 minutos. Cuando esta ventana expira, la autorización puede fallar con el código de error de Denuvo `88500005`; actualiza los datos del ticket antes de volver a intentarlo.
- Prioridad de AppTicket: la autorización de Denuvo requiere un ticket local explícito. Fuera de la autorización, se prefieren los tickets configurados mediante `setAppTicket` o ya guardados; solo si faltan se utiliza el ticket falsificado del ConfigStore local.
- Prioridad de SteamID: primero lee el `SteamID` guardado; si falta, lo analiza a partir del `AppTicket` explícito. En Windows se utiliza `HKCU\Software\Valve\Steam\Apps\<AppId>`. El backend de Linux aún no está implementado.
- En Windows, utiliza `addprocess` y `forcedenuvo` solo si falla la detección automática (ver el ejemplo Lua). Siguen siendo necesarios `addappid` y tickets válidos. Eliminar o recargar el archivo Lua retira sus ajustes; reinicia el juego para actualizar su detección.
- La detección estructural usa secciones grandes, ejecutables y escribibles con alta entropía como heurística; también puede detectar otros protectores.

#### Extracción de tickets con `extract_tickets`

La herramienta de Windows `extract_tickets` vuelca las cadenas hexadecimales de `AppTicket` y `ETicket` que necesitas para `setAppTicket` / `setETicket`. Ejecútala en una máquina donde Steam esté abierto e iniciado sesión en una cuenta que sea **propietaria** del juego en cuestión.

1. Compila las herramientas (Revisa [Compilación](#compilación)); el binario se generará en `build/tools/Release/extract_tickets.exe`.
2. Ejecútalo pasando el AppId del juego como argumento (o ejecútalo sin argumentos e introduce el AppId cuando se te solicite):
   ```powershell
   extract_tickets.exe 1361510
   ```
3. La herramienta leerá la ruta de instalación de Steam desde el registro, cargará `steamclient64.dll` y escribirá todo dentro de una carpeta `<appid>/` junto al ejecutable:
   - `appticket.bin` — ticket bruto de propiedad de la aplicación (binario)
   - `eticket.bin` — ticket cifrado bruto de la aplicación (binario)
   - `tickets.txt` — resumen en texto plano con las cadenas hexadecimales:
     ```
     appid:1361510
     appticket(184 bytes):14000000...
     eticket(143 bytes):...
     ```
   Si un ticket no se puede obtener, se reportará como
   `appticket:null` / `eticket:null`.
4. Pega las cadenas hexadecimales de `tickets.txt` en tu configuración de Lua:
   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **Nota:** Los tickets solo son válidos cuando se extraen de una cuenta que **realmente posee** el juego.

### Estadísticas y logros

- Activa las estadísticas y los logros para los juegos que no poseas.
- Utiliza `setStat(appid, "steamid")` para configurar de qué SteamID se deben extraer los datos de los logros.
- Si no hay ningún `setStat` configurado para una aplicación, OpenSteamTool consulta `https://stats.opensteamtool.com/{appid}` cuando `[stats] enable_api = true` (valor predeterminado).
- Prioridad: `setStat` > API de estadísticas cuando está habilitada y devuelve un valor válido > SteamID predefinido `76561198028121353`.

### Online Fix(Reparacion para habilitar el Online)

- Añade `-onlinefix` a los parámetros de lanzamiento de Steam para habilitar el juego en línea basado en el AppId 480 en juegos que utilizan emparejamiento (matchmaking) por salas (lobbies). La limitación actual es que solo se puede ejecutar uno de estos juegos a la vez. Para revertirlo, simplemente elimina -onlinefix de los parámetros de lanzamiento; el juego en línea volverá a la normalidad en el próximo inicio.
- Si el juego se cierra o falla la comprobación del AppID con `-onlinefix`, prueba `-onlinefix -realappid`. Esto mantiene el AppID real en `GetAppID()` incluso después de inicializar SteamNetworkingSockets; la sesión de OnlineFix sigue usando 480. Úsalo solo cuando sea necesario: algunos juegos P2P requieren que `GetAppID()` devuelva 480. `-realappid` por sí solo no tiene efecto ni garantiza la compatibilidad en línea.

### Guardado en la nube

- La redirección de guardados para juegos añadidos mediante Lua está disponible a través de la versión adaptada de [CloudRedirect](https://github.com/ehgen0ng/CloudRedirect), desactivada por defecto.
- Usa la biblioteca para tu plataforma de ese repositorio y actívala en `[cloud]`. Los proveedores de almacenamiento se configuran mediante CloudRedirect; reinicia Steam después de cambiar `[cloud]`.

## Uso

Cierra Steam por completo antes de instalar o actualizar. Descarga el paquete Release para tu plataforma o compílalo siguiendo [Compilación](#compilación). Usa un paquete Debug para obtener registros.

### Directorio de datos

| Plataforma | Directorio predeterminado |
|------------|---------------------------|
| Windows | `%LOCALAPPDATA%\OpenSteamTool` |
| macOS | `~/Library/Application Support/OpenSteamTool` |

La variable de entorno `OST_DATA_DIR` permite cambiar el directorio predeterminado. A continuación, `<directorio de datos>` se refiere a esta ubicación; los scripts Lua, registros, cachés de metadatos y plugins se guardan en `lua`, `logs`, `cache` y `plugins`, respectivamente.

### Windows (x64)

1. Copia `dwmapi.dll` y `xinput1_4.dll` al directorio raíz de Steam, junto a `steam.exe`.
2. Coloca `OpenSteamTool.dll` en `<directorio de datos>/bin/`; crea el directorio si no existe.
3. Coloca los scripts Lua en `<directorio de datos>/lua/` e inicia Steam.

### macOS (Apple Silicon)

1. Extrae el paquete macOS arm64 y mantén `steam-inject.sh` junto a `libOpenSteamTool.dylib`.
2. Con Steam instalado en `/Applications/Steam.app`, ejecuta `bash ./steam-inject.sh` desde el directorio extraído.
3. Coloca los scripts Lua en `<directorio de datos>/lua/`; se cargan automáticamente.

El script hace una copia de seguridad y vuelve a firmar el ejecutable de Steam de forma ad hoc antes de cargar OpenSteamTool. Para restaurar la firma original, cierra Steam y ejecuta `bash ./steam-inject.sh --restore`.

### Ejemplo de Lua

```lua
addappid(1361510) -- desbloquea el juego con appid 1361510

addappid(1361511, 0,"5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- desbloquea el juego con appid 1361511, la clave del depósito (depotKey) es "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0"

addtoken(1361510,"2764735786934684318") -- añade el token de acceso ("2764735786934684318") para el juego con appid 1361510
-- Ya no está soportado:
--pinApp(1361510) -- fija el juego con appid 1361510 para evitar que se actualice

setManifestid(1361511,"5656605350306673283") -- fija depotid:1361511 manifest_gid:5656605350306673283, el tamaño por defecto es 0
setManifestid(1361511,"5656605350306673283", 12345678) -- lo mismo, pero con un tamaño explícito

setAppTicket(1361510,"0100000000000000...") -- escribe AppTicket en el almacén de credenciales; en Windows: HKCU\Software\Valve\Steam\Apps\1361510\AppTicket

setETicket(1361510,"0100000000000000...") -- escribe ETicket en el almacén de credenciales; en Windows: HKCU\Software\Valve\Steam\Apps\1361510\ETicket

-- Ajustes opcionales de Denuvo en Windows; no hacen falta si la detección funciona:
--addprocess(1361510, "Game.exe") -- Asocia el proceso si no se puede resolver el AppID (sin distinguir mayúsculas)
--forcedenuvo(1361510) -- Solo para un juego Denuvo confirmado que no se detecte automáticamente

setStat(1361510, "76561197960287930") -- utiliza los datos de logros del SteamID especificado para el appid 1361510
-- Si no se configura, se utiliza la API de estadísticas cuando está habilitada; de lo contrario se usa el SteamID por defecto 76561198028121353.
```

Los nombres de todas las funciones **no distinguen entre mayúsculas y minúsculas**. `setAppTicket`, `setappticket`, `SetAppticket`, `SETAPPTICKET`, etc., son todas equivalentes. Lo mismo se aplica a cada función registrada (`addAppId`, `AddToken`, `SETManifestid`, etc.).

### Configuración (opcional)

Copia `opensteamtool.example.toml` a `<directorio de datos>/opensteamtool.toml`.
Si no se encuentra ningún archivo de configuración, se utilizarán los valores predeterminados integrados; no se creará ninguno de forma automática.
El archivo se supervisa mientras Steam está en ejecución; ajustes como el nivel de registro y los directorios Lua se recargan en caliente. Reinicia Steam después de cambiar `[cloud]`.

```toml
[log]
# Solo para compilaciones de depuración (Debug). Niveles: trace, debug, info, warn, error
level = "debug"

[manifest]
# API ascendente para los códigos de solicitud de manifiestos de depósito. Opciones: "opensteamtool", "steamrun", "wudrm"
url = "opensteamtool"

# Tiempos de espera HTTP para obtener códigos de solicitud (milisegundos)
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# Consulta https://stats.opensteamtool.com/{appid} cuando no existe setStat en Lua.
# Prioridad: setStat > API de estadísticas > SteamID predefinido.
enable_api = true

# Directorios adicionales de configuración de Lua (opcional).
# Se cargan en el orden de paths; <directorio de datos>/lua se carga al final.
[lua]
paths = []

[inject]
# Inyección opcional en juegos (solo Windows); rutas relativas al directorio de datos.
enabled = false
# library_x64 = "plugins/OpenSteamTool.GameHook.x64.dll"
# library_x86 = "plugins/OpenSteamTool.GameHook.x86.dll"

[cloud]
# Redirección opcional mediante CloudRedirect; reinicia Steam después de los cambios.
# Proporciona la biblioteca por separado; CloudRedirect configura el almacenamiento.
enabled = false
# library = "plugins/cloud_redirect.dll"   # Valor predeterminado en Windows
# library = "plugins/cloud_redirect.dylib" # Valor predeterminado en macOS

# Espejo (mirror) de metadatos opcional. Consulta "Compatibilidad con versiones de Steam" más abajo.
[remote]
# url_template = "https://tu.servidor/{channel}/{component}/{sha256}.toml"
```
### Manifiestos y códigos de solicitud

Cuando Steam solicita un depot configurado en Lua, se utiliza primero el manifiesto local. Si falta, OpenSteamTool consulta `https://manifest.luastools.xyz/m/{depotId}/{manifestId}` y guarda el manifiesto completo en `depotcache` de Steam. Si no está archivado, falla la red o el manifiesto no es válido, se continúa con el flujo existente de códigos de solicitud.

Sin `setManifestid` se utiliza la versión solicitada por Steam; con él se utiliza la versión indicada. El servicio solo proporciona manifiestos. Sigue siendo necesaria una configuración Lua con el juego, los depots y las claves de descifrado correctas.

Prioridad de códigos de solicitud: `fetch_manifest_code_ex` > `fetch_manifest_code` > la API seleccionada en `[manifest] url`. Un error de Lua o un resultado `nil` hace que se pruebe la siguiente alternativa. Estas funciones devuelven códigos de solicitud, no archivos de manifiesto.

#### `fetch_manifest_code(gid)`

Función básica que recibe únicamente el GID del manifiesto.

#### `fetch_manifest_code_ex(app_id, depot_id, gid)` *(recomendado)*

Función extendida que recibe `app_id`, `depot_id` y `gid`. Permite construir endpoints de API que requieran la identificación de la aplicación.

El entorno de ejecución (runtime) en C++ proporciona dos funciones auxiliares de Lua:

| Function | Signature | Returns |
|----------|-----------|---------|
| `http_get`  | `http_get(url [, headers])`       | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` es una tabla opcional: `{["Key"]="Value", ...}`.

### Compatibilidad con versiones de Steam

Tanto en Windows como en macOS, OpenSteamTool utiliza el SHA-256 de los archivos de los módulos de Steam para buscar los metadatos de patrones e IPC incluidos en el programa.

- Windows: compara `steamclient64.dll` y `steamui.dll`. Si no hay datos incluidos disponibles, consulta primero las fuentes remotas y después la caché local del mismo SHA-256.
- macOS: compara `steamclient.dylib` y `steamui.dylib`. Actualmente solo utiliza los datos incluidos; si no hay coincidencia, informa de una versión incompatible y desactiva las funciones afectadas.

En Windows, cuando los datos integrados no están disponibles, se consultan estas cinco fuentes remotas en paralelo (para los canales `pattern` e `ipc`):

1. **GitHub raw oficial** — `OpenSteam001/steam-monitor`.
2. **CDN jsDelivr oficial** — el mismo repositorio mediante CDN.
3. **GitHub raw comunitario** — `madoiscool/steam-monitor`.
4. **CDN jsDelivr comunitario** — el repositorio comunitario mediante CDN.
5. **LuaTools** — `https://git.lua.tools/luatools/steam-monitor`.

Por defecto, se utilizan los datos válidos de la primera fuente que responda correctamente y se actualiza la caché local. Si ninguna fuente remota está disponible, se utiliza la caché correspondiente al SHA-256 actual: `<directorio de datos>/cache/pattern/<component>/<sha256>.toml` (`ipc` para metadatos IPC).

Los datos integrados se actualizan con el programa. Cuando se necesitan fuentes remotas, cada inicio sigue obteniendo los datos más recientes sin tener que limpiar la caché manualmente.

Un **HTTP 404**, un error de red o un TOML inválido de una fuente no impide que las otras funcionen. Si todas las fuentes remotas y la caché del mismo SHA fallan, aparece una ventana emergente con el nombre de la DLL, su SHA-256, la ruta de caché esperada y la URL de origen. Solo se desactivan los hooks vinculados a esa DLL; el resto de OpenSteamTool sigue funcionando.

También puedes colocar manualmente un TOML de patrones en la caché si conoces la estructura de una compilación; debe llamarse `<sha256>.toml`. La caché de reserva lo utilizará cuando no haya datos integrados disponibles y fallen todas las fuentes remotas.

#### Uso de un espejo (mirror) diferente

Para utilizar un espejo privado o servidor de intranet, configura una plantilla de URL completa. Durante la búsqueda remota, la fuente personalizada se consulta junto con las predeterminadas, pero tiene prioridad. Solo si falla o agota su tiempo de espera se utiliza la primera alternativa que responda correctamente.

La plantilla debe incluir obligatoriamente `{channel}`, `{component}` y `{sha256}`. Los canales utilizados actualmente son `pattern` e `ipc`.

```toml
[remote]
url_template = "https://tu.servidor/{channel}/{component}/{sha256}.toml"
# url_template = "https://fast.jsdelivr.net/gh/OpenSteam001/steam-monitor@{channel}/{component}/{sha256}.toml"
```

### Registro de depuración

Las compilaciones Debug escriben registros por módulo en `<directorio de datos>/logs/`:

| Archivo | Fuente | Contenido |
|---------|--------|-----------|
| `main.log`          | General | Inicialización, carga de configuración, análisis de Lua, utilidades |
| `ipc.log`           | `LOG_IPC_*` | Comandos IPC, despacho de InterfaceCall, suplantación (spoofing) |
| `netpacket.log`     | `LOG_NETPACKET_*` | Envío/recepción de paquetes de red, despacho de eMsg |
| `manifest.log`      | `LOG_MANIFEST_*` | Descarga de manifiestos, fetch_manifest_code, vinculación de manifiestos |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Inyección de claves de descifrado de depósitos (depots) |
| `keyvalue.log`      | `LOG_KEYVALUE_*` | Parcheo de KeyValues (vinculación de manifiestos) |
| `misc.log`          | `LOG_MISC_*` | Captura de punteros del motor, sugerencias de AppId |
| `achievement.log`   | `LOG_ACHIEVEMENT_*` | Solicitudes/respuestas de UserStats, suplantación de steamid |
| `pics.log`          | `LOG_PICS_*` | Inyección de tokens de acceso PICS |
| `package.log`       | `LOG_PACKAGE_*` | Inyección de paquetes, eventos de FileWatcher |
| `onlinefix.log`     | `LOG_ONLINEFIX_*` | Parche en línea (suplantación del AppId 480) |
| `richpresence.log`  | `LOG_RICHPRESENCE_*` | Construcción e inyección de paquetes Rich Presence |
| `steamui.log`       | `LOG_STEAMUI_*` | Diagnósticos de hooks de SteamUI |
| `pipe.log`          | `LOG_PIPE_*` | Handshakes de pipe, inspección de procesos, autorización de Denuvo, inyección de bibliotecas |
| `platform.log`      | `LOG_PLATFORM_*` | Diagnósticos de utilidades de plataforma, incluidas operaciones sobre procesos remotos |

El nivel de registro se controla mediante `[log] level` en `opensteamtool.toml`.

## Compilación

### Requisitos

- CMake 3.20+ y Git; la primera configuración descarga las dependencias.
- Windows: Windows 10/11 y Visual Studio 2022 con MSVC (herramientas x64).
- macOS: Apple Silicon y Xcode Command Line Tools con soporte para C++20.

### Requisitos de ejecución

- Windows: si no hay metadatos integrados disponibles, se consultan las fuentes remotas y después la caché del mismo SHA-256 (ver [Compatibilidad con versiones de Steam](#compatibilidad-con-versiones-de-steam)).
- macOS: los metadatos incluidos deben corresponder a la versión actual de Steam.

### Compilación rápida

Windows, en una terminal con las herramientas de MSVC disponibles:

```powershell
build.bat
```

macOS:

```bash
./build.sh
```

Ambos scripts compilan Release y Debug por defecto. Después de compilar en macOS, ejecuta `bash tools/macos/steam-inject.sh --config Release` para cargar el archivo correspondiente.

### Archivos de salida

- Windows Debug: `build/Debug/OpenSteamTool.dll`, `build/Debug/dwmapi.dll`, `build/Debug/xinput1_4.dll`.
- Windows Release: `build/Release/OpenSteamTool.dll`, `build/Release/dwmapi.dll`, `build/Release/xinput1_4.dll`.
- macOS Debug: `build/macos/Debug/libOpenSteamTool.dylib`, con símbolos de depuración `.dSYM`.
- macOS Release: `build/macos/Release/libOpenSteamTool.dylib`.

## Descargo de responsabilidad

Este proyecto se proporciona únicamente con fines de investigación y educativos. Eres responsable de cumplir con las leyes locales, los términos de servicio de la plataforma y las licencias de software correspondientes.

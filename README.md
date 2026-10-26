<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Open-Source Steam Unlock Tool</strong>
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
      <img src="https://flagcdn.com/w40/us.png" width="22" alt="United States flag">
      English
    </a>
    &nbsp;|&nbsp;
    <a href="README_ES.md">
      <img src="https://flagcdn.com/w40/es.png" width="22" alt="Spain flag">
      Español
    </a>
    &nbsp;|&nbsp;
    <a href="README_ZH.md">
      <img src="https://flagcdn.com/w40/cn.png" width="22" alt="China flag">
      中文
    </a>
  </p>
</div>

## Feature

### Core Unlocks

- Unlock an unlimited number of unowned games.
- Unlock all DLCs for unowned games.
- Support auto load depot decryption keys from Lua config.
- Download archived manifests from `manifest.luastools.xyz` into Steam's `depotcache`, falling back to the existing request-code flow on failure.
- Obtain manifest request codes through the `opensteamtool` / `steamrun` / `wudrm` upstream APIs (default: `opensteamtool`), or a custom Lua endpoint (see [Manifests and request codes](#manifests-and-request-codes)).
- Support downloading protected games or DLCs that require an access token.
- Support binding manifest to prevent specific games from being updated.

### Hot Reload

- Adding, modifying, deleting, or overwriting `.lua` files in any watched directory automatically triggers a reload. No restart, no offline/online toggle needed.

### Injection

- Windows supports optional game-process library injection through `[inject]` in `opensteamtool.toml`; this feature is not implemented on macOS.
- Configure `enabled`, `library_x64`, and `library_x86`; the injected library must match the target process architecture. Paths may be absolute or relative to the data directory.

### Family Sharing and Remote Play

- Bypass Steam Family Sharing restrictions for games that have been added to the library with `addappid` in Lua. All accounts in the Steam Family that participate in sharing must use OpenSteamTool for this to work.

### Compatible with games protected by Denuvo and SteamStub

- Denuvo detection and authorization run only on Windows. macOS includes ticket storage and shared ticket handling, but does not implement this detection and authorization flow.
- SteamStub-only games do not require configuring `AppTicket`. OpenSteamTool can reuse Steam's local ConfigStore ticket and forge the requested AppId through a SteamDRMP off-by-four ticket parsing vulnerability, without injecting into the game process.
- Denuvo-protected games still require explicit ticket data. OpenSteamTool stores `AppTicket` and `ETicket` through the platform credential store.
- Use `setAppTicket(appid, "hex")` and `setETicket(appid, "hex")` in Lua config to write these values to the platform credential store automatically.
- Denuvo verification has a 30-minute validity window. After this window expires, authorization may fail with Denuvo error code `88500005`; refresh the ticket data before retrying.
- AppTicket priority: Denuvo authorization requires an explicit local ticket. Outside authorization, tickets configured by `setAppTicket` or already cached are still preferred; the forged local ConfigStore ticket is used only when no explicit ticket is available.
- SteamID priority: read cached `SteamID` first; if missing, parse from explicit `AppTicket`. On Windows, the credential store backend currently uses `HKCU\Software\Valve\Steam\Apps\<AppId>`. The Linux backend is not implemented yet.
- On Windows, use `addprocess` and `forcedenuvo` only when automatic detection misses (see the Lua example below). `addappid` and valid tickets are still required. Removing or reloading a Lua file withdraws its settings; restart a running game to refresh its detection result.
- Structural detection uses large, writable/executable, high-entropy sections as a fallback heuristic and may also match other protectors.

#### Extracting tickets with `extract_tickets`

The Windows `extract_tickets` tool dumps the `AppTicket` and `ETicket` hex strings you need for `setAppTicket` / `setETicket`. Run it on a machine where Steam is running and logged into an account that **owns** the target game.

1. Build the tools (see [Build](#build)); the binary lands in `build/tools/Release/extract_tickets.exe`.
2. Run it with the target AppId (or run it with no argument and type the AppId when prompted):
   ```powershell
   extract_tickets.exe 1361510
   ```
3. It reads the Steam install path from the registry, loads `steamclient64.dll`, and writes everything into an `<appid>/` folder next to the executable:
   - `appticket.bin` — raw app ownership ticket (binary)
   - `eticket.bin` — raw encrypted app ticket (binary)
   - `tickets.txt` — plain-text summary with the hex strings:
     ```
     appid:1361510
     appticket(184 bytes):14000000...
     eticket(143 bytes):...
     ```
   A ticket that could not be obtained is reported as `appticket:null` / `eticket:null`.
4. Paste the hex strings from `tickets.txt` into your Lua config:
   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **Note:** Tickets are only valid when extracted from an account that **genuinely owns** the game.

### Stats and Achievements

- Enable stats and achievements for unowned games.
- Uses `setStat(appid, "steamid")` to configure which SteamID's achievement data to pull.
- If no `setStat` is configured for an app, OpenSteamTool queries `https://stats.opensteamtool.com/{appid}` when `[stats] enable_api = true` (default).
- Priority: `setStat` > stats API when enabled and valid > hardcoded preset SteamID `76561198028121353`.

### Online Fix

- Add `-onlinefix` to the Steam launch parameters to enable 480-based online play in games that use lobby matchmaking. The current limitation is that only one such game can run at a time.To revert, simply remove -onlinefix from the launch parameters — online play returns to normal on the next launch.

### Cloud saves

- Optional cloud-save redirection for games added through Lua is available through the companion [CloudRedirect](https://github.com/ehgen0ng/CloudRedirect), disabled by default.
- Use the library for your platform from that repository and enable it under `[cloud]`. Cloud storage providers are configured by CloudRedirect; restart Steam after changing `[cloud]`.

## Usage

Fully exit Steam before installing or updating. Download the Release package for your platform, or compile it as described under [Build](#build). Use a Debug package for logging.

### Data directory

| Platform | Default directory |
|----------|-------------------|
| Windows | `%LOCALAPPDATA%\OpenSteamTool` |
| macOS | `~/Library/Application Support/OpenSteamTool` |

Set the `OST_DATA_DIR` environment variable to override the default. Below, `<data directory>` refers to this location; Lua scripts, logs, metadata caches, and plugins go in its `lua`, `logs`, `cache`, and `plugins` subdirectories.

### Windows (x64)

1. Copy `dwmapi.dll` and `xinput1_4.dll` to the Steam root directory, next to `steam.exe`.
2. Place `OpenSteamTool.dll` in `<data directory>/bin/`, creating the directory if needed.
3. Place Lua scripts in `<data directory>/lua/`, then launch Steam.

### macOS (Apple Silicon)

1. Extract the macOS arm64 package and keep `steam-inject.sh` beside `libOpenSteamTool.dylib`.
2. With Steam installed at `/Applications/Steam.app`, run `bash ./steam-inject.sh` from the extracted directory.
3. Place Lua scripts in `<data directory>/lua/`; they are loaded automatically.

The launcher backs up and ad-hoc re-signs Steam's executable before loading OpenSteamTool. To restore the original signature, exit Steam and run `bash ./steam-inject.sh --restore`.

### Lua example

```lua
addappid(1361510) -- unlock game with appid 1361510

addappid(1361511, 0,"5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- unlock game with appid 1361511 depotKey is "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0"

addtoken(1361510,"2764735786934684318") -- add access token ("2764735786934684318") for game with appid 1361510
-- No Longer Supported:
--pinApp(1361510) -- pin game with appid 1361510 to prevent it from being updated

setManifestid(1361511,"5656605350306673283") -- pin depotid:1361511 manifest_gid:5656605350306673283, size defaults to 0
setManifestid(1361511,"5656605350306673283", 12345678) -- same but with explicit size

setAppTicket(1361510,"0100000000000000...") -- write AppTicket to the credential store; on Windows: HKCU\Software\Valve\Steam\Apps\1361510\AppTicket

setETicket(1361510,"0100000000000000...") -- write ETicket to the credential store; on Windows: HKCU\Software\Valve\Steam\Apps\1361510\ETicket

-- Optional Windows Denuvo compatibility settings; not needed when detection works:
--addprocess(1361510, "Game.exe") -- Match by process name when AppID cannot be resolved automatically (case-insensitive)
--forcedenuvo(1361510) -- Enable only for a confirmed Denuvo game that automatic detection misses

setStat(1361510, "76561197960287930") -- use the specified SteamID's achievement data for appid 1361510
-- If not configured, the stats API is used when enabled; otherwise default SteamID 76561198028121353 is used.
```

All function names are **case-insensitive**. `setAppTicket`, `setappticket`, `SetAppticket`, `SETAPPTICKET` etc. are all equivalent. The same applies to every registered function (`addAppId`, `AddToken`, `SETManifestid`, etc.).

### Configuration (optional)

Copy `opensteamtool.example.toml` to `<data directory>/opensteamtool.toml`.
If no config file is found, built-in defaults are used — no auto-creation.
The file is watched while Steam is running; settings such as log levels and Lua directories are hot-reloaded. Restart Steam after changing `[cloud]`.

```toml
[log]
# Debug build only.  Level: trace, debug, info, warn, error
level = "debug"

[manifest]
# Upstream API for depot manifest request codes.  Options: "opensteamtool", "steamrun", "wudrm"
url = "opensteamtool"

# HTTP timeouts for manifest request-code queries (milliseconds)
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# Query https://stats.opensteamtool.com/{appid} when no Lua setStat override exists.
# Priority: setStat > stats API > hardcoded preset SteamID.
enable_api = true

# Additional Lua config directories (optional).
# Load in paths array order, then load <data directory>/lua last.
[lua]
paths = []

[inject]
# Optional game-process injection (Windows only); relative paths use the data directory.
enabled = false
# library_x64 = "plugins/OpenSteamTool.GameHook.x64.dll"
# library_x86 = "plugins/OpenSteamTool.GameHook.x86.dll"

[cloud]
# Optional CloudRedirect cloud-save redirection; restart Steam after changes.
# Supply the platform library separately; CloudRedirect configures storage providers.
enabled = false
# library = "plugins/cloud_redirect.dll"   # Windows default
# library = "plugins/cloud_redirect.dylib" # macOS default

# Optional metadata mirror. See "Steam version compatibility" below.
[remote]
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

### Manifests and request codes

When Steam requests a depot configured in Lua, an existing local manifest is used first. If missing, OpenSteamTool tries `https://manifest.luastools.xyz/m/{depotId}/{manifestId}` and stores the complete manifest in Steam's `depotcache`. Missing archives, network failures, or invalid manifests fall back to the existing request-code flow.

Without `setManifestid`, Steam's requested version is used; with it, the specified version is used. The service supplies manifests only. A Lua configuration with the game, depots, and correct decryption keys is still required.

Request-code priority: `fetch_manifest_code_ex` > `fetch_manifest_code` > the upstream selected by `[manifest] url`. A Lua error or `nil` result continues to the next fallback. These functions return request codes, not manifest files.

#### `fetch_manifest_code(gid)`

Basic function that receives only the manifest GID.

#### `fetch_manifest_code_ex(app_id, depot_id, gid)` *(recommended)*

Extended function that receives `app_id`, `depot_id`, and `gid`. Allows constructing API endpoints that require app identification.

The C++ runtime provides two Lua helpers:

| Function | Signature | Returns |
|----------|-----------|---------|
| `http_get`  | `http_get(url [, headers])`       | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` is an optional table: `{["Key"]="Value", ...}`.

### Steam version compatibility

On both Windows and macOS, OpenSteamTool uses the SHA-256 of Steam module files to look up bundled pattern and IPC metadata.

- Windows: matches `steamclient64.dll` and `steamui.dll`. When bundled data is unavailable, remote sources are tried first, followed by the local cache for the same SHA-256.
- macOS: matches `steamclient.dylib` and `steamui.dylib`. Only bundled data is currently used; a missing match reports an unsupported version and disables the affected functionality.

On Windows, these five remote sources are requested concurrently when embedded data is unavailable (shared by the `pattern` and `ipc` channels):

1. **Official GitHub raw** — `OpenSteam001/steam-monitor`.
2. **Official jsDelivr CDN** — the same repository via CDN.
3. **Community GitHub raw** — `madoiscool/steam-monitor`.
4. **Community jsDelivr CDN** — the community repository via CDN.
5. **LuaTools** — `https://git.lua.tools/luatools/steam-monitor`.

By default, the fastest successful source supplies the valid data and updates the local cache. If all remote sources are unavailable, loading falls back to the cache matching the current SHA-256: `<data directory>/cache/pattern/<component>/<sha256>.toml` (`ipc` for IPC metadata).

Embedded data is updated with the program. When remote sources are needed, each launch still fetches the latest upstream data without requiring users to clear the cache.

An **HTTP 404**, network error, or invalid TOML from one source does not prevent other sources from succeeding. If all remote sources and the exact-SHA cache are unavailable, a one-shot popup appears with the unmatched DLL name, its SHA-256, the expected cache path, and the upstream URL. Only the hooks tied to that DLL are disabled — the rest of OpenSteamTool keeps working.

You can also drop a pattern TOML into the cache directory manually if you know the layout for a given build; the file name must be `<sha256>.toml`. The cache fallback will pick it up when embedded data is unavailable and all remote sources fail.

#### Using a different mirror

To use a private mirror or intranet server, configure a full URL template. During remote lookup, the custom source is requested alongside the default sources but takes priority. The fastest successful backup is used only if the custom source fails or times out.

The template must include `{channel}`, `{component}`, and `{sha256}`. Channels currently used are `pattern` and `ipc`.

```toml
[remote]
url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
# url_template = "https://fast.jsdelivr.net/gh/OpenSteam001/steam-monitor@{channel}/{component}/{sha256}.toml"
```

### Debug logging

Debug builds write per-module log files under `<data directory>/logs/`:

| File | Source | Content |
|------|--------|---------|
| `main.log`          | General | Init, config loading, Lua parsing, utilities |
| `ipc.log`           | `LOG_IPC_*` | IPC commands, InterfaceCall dispatch, spoofing |
| `netpacket.log`     | `LOG_NETPACKET_*` | Network packet send/recv, eMsg dispatch |
| `manifest.log`      | `LOG_MANIFEST_*` | Manifest download, `fetch_manifest_code`, manifest binding |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot decryption key injection |
| `keyvalue.log`      | `LOG_KEYVALUE_*` | KeyValues patching (manifest binding) |
| `misc.log`          | `LOG_MISC_*` | Engine pointer capture, AppId hints |
| `achievement.log`   | `LOG_ACHIEVEMENT_*` | UserStats requests/responses, steamid spoofing |
| `pics.log`          | `LOG_PICS_*` | PICS access token injection |
| `package.log`       | `LOG_PACKAGE_*` | Package injection, FileWatcher events |
| `onlinefix.log`     | `LOG_ONLINEFIX_*` | Online fix (480 AppId spoofing) |
| `richpresence.log`  | `LOG_RICHPRESENCE_*` | Rich Presence packet construction and injection |
| `steamui.log`       | `LOG_STEAMUI_*` | SteamUI hook diagnostics |
| `pipe.log`          | `LOG_PIPE_*` | Pipe handshakes, process inspection, Denuvo authorization, library injection |
| `platform.log`      | `LOG_PLATFORM_*` | Platform helper diagnostics, including remote-process operations |

The log level is controlled by `[log] level` in `opensteamtool.toml`.

## Build

### Requirements

- CMake 3.20+ and Git; the first configuration downloads dependencies.
- Windows: Windows 10/11 and Visual Studio 2022 with MSVC (x64 toolchain).
- macOS: Apple Silicon and Xcode Command Line Tools with C++20 support.

### Runtime requirements

- Windows: when embedded metadata is unavailable, remote sources are tried before the cache for the same SHA-256 (see [Steam version compatibility](#steam-version-compatibility)).
- macOS: bundled metadata must match the current Steam version.

### Quick build

Windows, in a terminal with the MSVC toolchain available:

```powershell
build.bat
```

macOS:

```bash
./build.sh
```

Both scripts build Release and Debug by default. After building from source on macOS, run `bash tools/macos/steam-inject.sh --config Release` to load the matching artifact.

### Output

- Windows Debug: `build/Debug/OpenSteamTool.dll`, `build/Debug/dwmapi.dll`, `build/Debug/xinput1_4.dll`.
- Windows Release: `build/Release/OpenSteamTool.dll`, `build/Release/dwmapi.dll`, `build/Release/xinput1_4.dll`.
- macOS Debug: `build/macos/Debug/libOpenSteamTool.dylib`, with `.dSYM` debug symbols.
- macOS Release: `build/macos/Release/libOpenSteamTool.dylib`.

## Disclaimer

This project is provided for research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.

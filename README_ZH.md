<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>开源 Steam 解锁工具</strong>
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

## 功能特性

### 核心解锁

- 解锁任意数量未拥有的游戏
- 解锁未拥有游戏的所有 DLC
- 支持从 Lua 配置自动加载仓库（depot）解密密钥
- 支持从 `manifest.luastools.xyz` 下载已收录的完整清单并写入 Steam 的 `depotcache`，失败时回退到原有请求码流程
- 支持通过 `opensteamtool` / `steamrun` / `wudrm` 上游 API 获取清单请求码（默认为 `opensteamtool`），或通过自定义 Lua 端点获取（参见 [Manifest 下载与请求码](#manifest-下载与请求码)）
- 支持下载需要访问令牌的保护游戏或 DLC
- 支持绑定 manifest 以防止特定游戏被更新

### 热重载

- 在任何监视目录中添加、修改、删除或覆盖 `.lua` 文件会自动触发重载。无需重启，无需切换离线/在线模式

### 注入

- Windows 支持通过 `opensteamtool.toml` 中的 `[inject]` 配置可选的游戏进程库注入；macOS 尚未实现此功能
- 配置 `enabled`、`library_x64` 和 `library_x86`；注入的库必须与目标进程架构匹配。路径可以是绝对路径，也可以是相对于数据目录的相对路径

### 家庭共享和远程同乐

- 绕过 Steam 家庭共享限制，适用于通过 Lua 中 `addappid` 添加到库的游戏。参与共享的 Steam 家庭中的所有账户都必须使用 OpenSteamTool 才能生效

### 兼容 Denuvo 和 SteamStub 保护的游戏

- Denuvo 检测与授权流程仅在 Windows 上启用；macOS 包含票据读写与共用处理，但未实现这套检测和授权流程
- 仅 SteamStub 保护的游戏不需要配置 `AppTicket`。OpenSteamTool 可以重用 Steam 本地 ConfigStore 令牌，通过 SteamDRMP 令牌解析漏洞伪造请求的 AppId，无需注入游戏进程
- Denuvo 保护的游戏仍需要显式令牌数据。OpenSteamTool 通过平台凭据存储保存 `AppTicket` 和 `ETicket`
- 在 Lua 配置中使用 `setAppTicket(appid, "hex")` 和 `setETicket(appid, "hex")` 自动将这些值写入平台凭据存储
- Denuvo 验证有 30 分钟有效窗口。过期后授权可能失败，显示 Denuvo 错误代码 `88500005`；重试前请刷新令牌数据
- AppTicket 优先级：Denuvo 授权期间只使用本地显式令牌；其他阶段也优先使用通过 `setAppTicket` 配置或已缓存的 `AppTicket`，缺失时才回退到伪造的本地 ConfigStore 令牌路径
- SteamID 优先级：优先读取缓存的 `SteamID`；若缺失，则从显式 `AppTicket` 解析。在 Windows 上，凭据存储后端当前使用 `HKCU\Software\Valve\Steam\Apps\<AppId>`。Linux 后端尚未实现
- Windows 下，自动检测未命中时可按需使用 `addprocess` 和 `forcedenuvo`（见下方 Lua 示例）。仍需 `addappid` 和有效令牌；删除或重载 Lua 会撤销该文件提供的设置，已运行的游戏需重启后重新检测
- 结构检测兜底根据大型、可写且可执行的高熵节判断，可能匹配其他保护程序

#### 使用 `extract_tickets` 提取令牌

Windows 工具 `extract_tickets` 会转储你需要的 `AppTicket` 和 `ETicket` 十六进制字符串，用于 `setAppTicket` / `setETicket`。在 Steam 运行并登录到**拥有**目标游戏的账户的机器上运行它。

1. 构建工具（参见 [构建](#构建)）；二进制文件位于 `build/tools/Release/extract_tickets.exe`
2. 使用目标 AppId 运行它（或不带参数运行，在提示时输入 AppId）：
   ```powershell
   extract_tickets.exe 1361510
   ```
3. 它从注册表读取 Steam 安装路径，加载 `steamclient64.dll`，并将所有内容写入可执行文件旁边的 `<appid>/` 文件夹：
   - `appticket.bin` — 原始应用所有权令牌（二进制）
   - `eticket.bin` — 原始加密应用令牌（二进制）
   - `tickets.txt` — 包含十六进制字符串的纯文本摘要：
     ```
     appid:1361510
     appticket(184 bytes):14000000...
     eticket(143 bytes):...
     ```
   无法获取的令牌报告为 `appticket:null` / `eticket:null`
4. 将 `tickets.txt` 中的十六进制字符串粘贴到你的 Lua 配置中：
   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **注意：** 令牌仅当从**真正拥有**游戏的账户提取时才有效

### 统计和成就

- 为未拥有的游戏启用统计和成就
- 使用 `setStat(appid, "steamid")` 配置拉取哪个 SteamID 的成就数据
- 如果某个应用未配置 `setStat`，当 `[stats] enable_api = true`（默认）时，OpenSteamTool 查询 `https://stats.opensteamtool.com/{appid}`
- 优先级：`setStat` > stats API（启用且有效时）> 硬编码预设 SteamID `76561198028121353`

### 在线修复

- 在 Steam 启动参数中添加 `-onlinefix` 以在需要使用大厅匹配的游戏里启用基于 480 的在线游戏。当前限制是同一时间只能运行一个这样的游戏。要撤销，只需从启动参数中移除 `-onlinefix` — 下次启动时在线游戏恢复正常

### 云存档

- 支持通过配套的 [CloudRedirect](https://github.com/ehgen0ng/CloudRedirect) 为 Lua 添加的游戏提供云存档重定向，默认关闭
- 使用该仓库的对应平台库，并在 `[cloud]` 中启用。云存储服务由 CloudRedirect 配置；修改 `[cloud]` 后需重启 Steam

## 使用方法

安装或更新前，请完全退出 Steam。可下载对应平台的 Release 包，或按 [构建](#构建) 一节自行编译；需要日志时使用 Debug 包。

### 数据目录

| 平台 | 默认目录 |
|------|----------|
| Windows | `%LOCALAPPDATA%\OpenSteamTool` |
| macOS | `~/Library/Application Support/OpenSteamTool` |

可通过环境变量 `OST_DATA_DIR` 覆盖默认目录。下文中的 `<数据目录>` 均指此目录；Lua、日志、元数据缓存和插件分别放在 `lua`、`logs`、`cache` 和 `plugins` 子目录中。

### Windows（x64）

1. 将 `dwmapi.dll` 和 `xinput1_4.dll` 复制到 Steam 根目录（与 `steam.exe` 同级）
2. 将 `OpenSteamTool.dll` 放入 `<数据目录>/bin/`，目录不存在时先创建
3. 将 Lua 脚本放入 `<数据目录>/lua/`，然后启动 Steam

### macOS（Apple Silicon）

1. 解压 macOS arm64 包，将 `steam-inject.sh` 与 `libOpenSteamTool.dylib` 保持在同一目录
2. 确认 Steam 安装在 `/Applications/Steam.app`，在解压目录运行 `bash ./steam-inject.sh`
3. 将 Lua 脚本放入 `<数据目录>/lua/`，文件会自动加载

启动脚本会备份 Steam 主程序并以 ad-hoc 方式重新签名，再加载 OpenSteamTool。需要恢复原签名时，退出 Steam 后运行 `bash ./steam-inject.sh --restore`。

### Lua 示例

```lua
addappid(1361510) -- 解锁 appid 为 1361510 的游戏

addappid(1361511, 0,"5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- 解锁 appid 为 1361511 的游戏，depotKey 为 "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0"

addtoken(1361510,"2764735786934684318") -- 为 appid 为 1361510 的游戏添加访问令牌 ("2764735786934684318")
-- 不再支持：
--pinApp(1361510) -- 固定 appid 为 1361510 的游戏以防止其被更新

setManifestid(1361511,"5656605350306673283") -- 固定 depotid:1361511 manifest_gid:5656605350306673283，大小默认为 0
setManifestid(1361511,"5656605350306673283", 12345678) -- 同上，但指定明确大小

setAppTicket(1361510,"0100000000000000...") -- 将 AppTicket 写入凭据存储；在 Windows 上：HKCU\Software\Valve\Steam\Apps\1361510\AppTicket

setETicket(1361510,"0100000000000000...") -- 将 ETicket 写入凭据存储；在 Windows 上：HKCU\Software\Valve\Steam\Apps\1361510\ETicket

-- 可选的 Windows Denuvo 兼容设置，正常识别时无需添加：
--addprocess(1361510, "Game.exe") -- 无法自动识别 AppID 时，按进程名匹配（不区分大小写）
--forcedenuvo(1361510) -- 已确认使用 Denuvo，但自动检测未命中时启用

setStat(1361510, "76561197960287930") -- 使用指定 SteamID 的成就数据用于 appid 1361510
-- 若未配置，启用时使用 stats API；否则使用默认 SteamID 76561198028121353
```

所有函数名**不区分大小写**。`setAppTicket`、`setappticket`、`SetAppticket`、`SETAPPTICKET` 等都是等价的。每个注册的函数都适用（`addAppId`、`AddToken`、`SETManifestid` 等）。

### 配置（可选）

将 `opensteamtool.example.toml` 复制为 `<数据目录>/opensteamtool.toml`。
若找不到配置文件，则使用内置默认值——不会自动创建。
配置文件在 Steam 运行时被监视；日志级别、Lua 目录等设置支持热重载。修改 `[cloud]` 后需重启 Steam。

```toml
[log]
# 仅调试构建。级别：trace、debug、info、warn、error
level = "debug"

[manifest]
# 仓库 manifest 请求码的上游 API。选项："opensteamtool"、"steamrun"、"wudrm"
url = "opensteamtool"

# 清单请求码请求的 HTTP 超时（毫秒）
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# 当没有 Lua setStat 覆盖时查询 https://stats.opensteamtool.com/{appid}
# 优先级：setStat > stats API > 硬编码预设 SteamID
enable_api = true

# 额外的 Lua 配置目录（可选）
# 按 paths 数组顺序加载，最后加载 <数据目录>/lua
[lua]
paths = []

[inject]
# 可选的游戏进程库注入（仅 Windows）；相对路径以数据目录为基准
enabled = false
# library_x64 = "plugins/OpenSteamTool.GameHook.x64.dll"
# library_x86 = "plugins/OpenSteamTool.GameHook.x86.dll"

[cloud]
# 可选的 CloudRedirect 云存档重定向，修改后需重启 Steam
# 需要另行提供对应平台的库；云存储服务由 CloudRedirect 配置
enabled = false
# library = "plugins/cloud_redirect.dll"   # Windows 默认路径
# library = "plugins/cloud_redirect.dylib" # macOS 默认路径

# 可选元数据镜像。参见下面的"Steam 版本兼容性"
[remote]
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

### Manifest 下载与请求码

Steam 请求 Lua 中已配置的 depot 时，优先使用本地缓存；缺失时尝试从 `https://manifest.luastools.xyz/m/{depotId}/{manifestId}` 下载完整清单，写入 Steam 的 `depotcache`。服务未收录、网络失败或清单无效时，继续原有请求码流程。

未配置 `setManifestid` 时使用 Steam 此次请求的版本；配置后使用指定版本。该服务只提供清单，仍需准备包含游戏、depot 和正确解密密钥的 Lua。

请求码优先级：`fetch_manifest_code_ex` > `fetch_manifest_code` > `[manifest] url` 指定的上游。Lua 函数失败或返回 `nil` 时继续回退；这两个函数返回请求码，不返回清单文件。

#### `fetch_manifest_code(gid)`

基础函数，只接收 manifest GID

#### `fetch_manifest_code_ex(app_id, depot_id, gid)` *（推荐）*

扩展函数，接收 `app_id`、`depot_id` 和 `gid`。允许构造需要应用识别的 API 端点

C++ 运行时提供两个 Lua 辅助函数：

| 函数 | 签名 | 返回值 |
|------|------|--------|
| `http_get`  | `http_get(url [, headers])`       | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` 是可选表：`{["Key"]="Value", ...}`

### Steam 版本兼容性

OpenSteamTool 在 Windows 和 macOS 上都会根据 Steam 模块文件的 SHA-256，查找随程序内置的模式和 IPC 元数据。

- Windows：匹配 `steamclient64.dll` 和 `steamui.dll`；内置数据不可用时，依次尝试远程源和同 SHA-256 的本地缓存
- macOS：匹配 `steamclient.dylib` 和 `steamui.dylib`；目前仅使用内置数据，未匹配时提示版本不兼容并停用相关功能

Windows 在内置数据不可用时，并发请求以下五个远程源（`pattern` 和 `ipc` 通道共用）：

1. **官方 GitHub raw** — `OpenSteam001/steam-monitor`
2. **官方 jsDelivr CDN** — 同一仓库的 CDN
3. **社区 GitHub raw** — `madoiscool/steam-monitor`
4. **社区 jsDelivr CDN** — 同一社区仓库的 CDN
5. **LuaTools** — `https://git.lua.tools/luatools/steam-monitor`

默认采用最快成功返回的有效数据，并更新本地缓存。所有远程源均不可用时，回退到匹配当前 SHA-256 的缓存：`<数据目录>/cache/pattern/<component>/<sha256>.toml`（IPC 使用 `ipc` 目录）。

内置数据随程序版本更新。使用远程源时，每次启动仍会获取上游最新数据，无需手动清除缓存。

某个源返回 **HTTP 404**、网络错误或无效 TOML，不会阻止其他源成功。所有远程源和同 SHA 缓存都不可用时，出现一次性弹窗，显示不匹配的 DLL 名称、其 SHA-256、预期缓存路径和上游 URL。仅禁用与该 DLL 相关的钩子——OpenSteamTool 的其余部分继续工作。

如果你知道给定版本的布局，也可以手动将模式 TOML 放入缓存目录；文件名必须为 `<sha256>.toml`。当内置数据不可用且远程全部失败时，缓存回退会拾取它。

#### 使用不同的镜像

要使用私有镜像或内网服务器，可配置完整的 URL 模板。远程查找时，自定义源与默认源同时请求，但优先使用自定义源；只有它失败或超时后，才采用最快成功的备用源。

模板必须包含 `{channel}`、`{component}` 和 `{sha256}`。当前使用的通道是 `pattern` 和 `ipc`

```toml
[remote]
url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
# url_template = "https://fast.jsdelivr.net/gh/OpenSteam001/steam-monitor@{channel}/{component}/{sha256}.toml"
```

### 调试日志

调试构建在 `<数据目录>/logs/` 下写入每个模块的日志文件：

| 文件 | 来源 | 内容 |
|------|------|------|
| `main.log` | 通用 | 初始化、配置加载、Lua 解析、工具 |
| `ipc.log` | `LOG_IPC_*` | IPC 命令、InterfaceCall 分发、欺骗 |
| `netpacket.log` | `LOG_NETPACKET_*` | 网络包发送/接收、eMsg 分发 |
| `manifest.log` | `LOG_MANIFEST_*` | Manifest 下载、`fetch_manifest_code`、manifest 绑定 |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | 仓库解密密钥注入 |
| `keyvalue.log` | `LOG_KEYVALUE_*` | KeyValues 补丁（manifest 绑定） |
| `misc.log` | `LOG_MISC_*` | 引擎指针捕获、AppId 提示 |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | UserStats 请求/响应、steamid 欺骗 |
| `pics.log` | `LOG_PICS_*` | PICS 访问令牌注入 |
| `package.log` | `LOG_PACKAGE_*` | 包注入、FileWatcher 事件 |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | 在线修复（480 AppId 欺骗） |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | 丰富状态包构造和注入 |
| `steamui.log` | `LOG_STEAMUI_*` | SteamUI 钩子诊断 |
| `pipe.log` | `LOG_PIPE_*` | 管道握手、进程检查、Denuvo 授权、库注入 |
| `platform.log` | `LOG_PLATFORM_*` | 平台助手诊断，包括远程进程操作 |

日志级别由 `opensteamtool.toml` 中的 `[log] level` 控制

## 构建

### 要求

- CMake 3.20+、Git；首次配置时需要联网下载依赖
- Windows：Windows 10/11，带有 MSVC（x64 工具链）的 Visual Studio 2022
- macOS：Apple Silicon，支持 C++20 的 Xcode Command Line Tools

### 运行时要求

- Windows：内置元数据不可用时，尝试远程源，再回退到同 SHA-256 的缓存（参见 [Steam 版本兼容性](#steam-版本兼容性)）
- macOS：使用与当前 Steam 版本匹配的内置元数据

### 快速构建

Windows（在可用 MSVC 工具链的终端中）：

```powershell
build.bat
```

macOS：

```bash
./build.sh
```

两个脚本默认都构建 Release 和 Debug。macOS 从源码构建后，可运行 `bash tools/macos/steam-inject.sh --config Release` 加载对应产物。

### 输出

- Windows Debug：`build/Debug/OpenSteamTool.dll`、`build/Debug/dwmapi.dll`、`build/Debug/xinput1_4.dll`
- Windows Release：`build/Release/OpenSteamTool.dll`、`build/Release/dwmapi.dll`、`build/Release/xinput1_4.dll`
- macOS Debug：`build/macos/Debug/libOpenSteamTool.dylib`，附带 `.dSYM` 调试符号
- macOS Release：`build/macos/Release/libOpenSteamTool.dylib`

## 免责声明

本项目仅供研究和教育目的使用。你负责遵守当地法律、平台服务条款和软件许可证。

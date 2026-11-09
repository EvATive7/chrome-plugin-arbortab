# ArborTab 开发指南

ArborTab 让 Chrome 原生的**垂直标签栏**变成树状标签（Windows x64）。由两部分组成：

- **扩展**（`extension/`，MV3）：维护标签树（父子、折叠），执行所有结构操作（`chrome.tabs.move` 等），并把显示状态推给原生模块。
- **原生模块**（`native/`，`version.dll` 代理）：放在 `chrome.exe` 同目录，被浏览器进程加载后用 MinHook 接管标签栏的布局、绘制、鼠标和拖拽，实现缩进、隐藏、折叠标记、拖放落点。扩展经 Native Messaging 桥（`arbortab-bridge.exe`）连到本浏览器的命名管道，WebSocket 作后备（见“通信协议”）。
- **偏移量**：原生模块所需的函数 RVA、字段偏移、vtable 等按 Chrome 版本由 `resolver/` 从官方 PDB 生成，CI 签名后发布到 `offsets` 分支，运行时按 PDB key 下载、验签并永久缓存（见“偏移量”）。

## 目录结构

```
VERSION             发布版本号的唯一来源（MAJOR.MINOR.PATCH）
extension/          MV3 扩展；src/background/tree.js 是纯树模型，test/ 是单元测试；icons/ 由 assets/logo/make.py 生成
native/             version.dll 与 arbortab-bridge.exe（CMake + MSVC）
  src/core          基础设施：偏移量（offsets、offsets_trust 验签/撤回、offsets_key.h 公钥）、crash_guard（安全模式）、日志、路径
  src/features      各功能的 hook
  src/link          通信：server（与传输无关的消息处理）、pipe_server、ws_server、pipe_io（与桥共用）、protocol.h 协议号
  bridge/           arbortab-bridge.exe（Native Messaging 宿主，只转发字节）
  third_party/      vendored MinHook、nlohmann/json
resolver/           resolve.py + spec.yaml：从 chrome.dll + PDB 生成偏移量 JSON（缺任何一项即失败）；spec.yaml 的 schema 即偏移量 schema
offsets/            offsets 分支的样例副本（运行时实际从 GitHub 下载）：index.json + win64/s<schema>/<key>.json
packaging/windows/  随原生包发给用户的 install/uninstall 脚本（.cmd 包装 .ps1，兼容 Windows PowerShell 5.1）与桥的宿主清单 com.arbortab.bridge.json
assets/             logo（logo/make.py 生成）、README 截图
tools/
  ci/               CI 用：query-chrome.py、extract-chrome.ps1、offsets_index.py（offsets 分支布局）、offsets_sign.py（签名/撤回）、check_repo.py（一致性检查）、package.py（打包）
  dev/              本地开发：build-native.ps1、dev.py/dev.ps1（一键重建+重启沙盒）、sandbox.py、nativecheck.py
  re/               逆向/符号：pdbinfo.py、symq.py、dis.py
  test/             e2e.py（沙盒 Chrome 中的端到端检查）
.github/workflows/  ci.yml（检查+构建）、release.yml（tag → 草稿 Release）、offsets.yml（每 6 小时发布新版 Chrome 的偏移量）
artifacts/ build/   本地产物（已 gitignore）
```

## 常用命令

```sh
# 构建原生模块 → artifacts/native/version.dll、arbortab-bridge.exe
pwsh -File tools/dev/build-native.ps1 [-Configuration Debug]

# 一键开发循环：构建 → 停沙盒 → 装 DLL、注册桥 → 启动 → 加载并重载扩展 → 从 native.log 核对构建与桥连接
pwsh -File tools/dev/dev.ps1 [-NoBuild]

# 停沙盒；--as-crash 让下次启动把这次当作异常退出（测安全模式）
uv run python tools/dev/sandbox.py kill [--as-crash]

# 扩展单元测试（树模型、协议检查）
node --test extension/test/*.test.mjs

# 版本号 / 多语言文案 / offsets 样例一致性（CI 同款）
uv run python tools/ci/check_repo.py

# 本地打包（与 release.yml 相同）
uv run python tools/ci/package.py --dll artifacts/native/version.dll --out dist

# 端到端（需沙盒 Chrome 已运行）
uv run --with websocket-client python tools/test/e2e.py tree|drop|tree-visual

# 直接驱动原生端点（不经扩展）/ 查看布局快照
uv run --with websocket-client python tools/dev/nativecheck.py --layout

# 生成偏移量
uv run --with pefile --with pyyaml python resolver/resolve.py --dll <chrome.dll> --pdb <chrome.dll.pdb> --out <key>.json

# 偏移量签名 / 校验 / 撤回（sign、revoke 需要 OFFSETS_SIGNING_KEY）
uv run --with cryptography python tools/ci/offsets_sign.py sign|verify <root>
uv run --with cryptography python tools/ci/offsets_sign.py revoke <root> <文件或 sha256>
```

原生模块日志：`%LOCALAPPDATA%\ArborTab\logs\native.log`。

## 工作约定

- **只动沙盒 Chrome，不碰本机安装的 Chrome**。沙盒目录由环境变量 `ARBORTAB_SANDBOX` 指定（必填，未设置时 `sandbox.py` 直接报错），CDP 端口 9334（`ARBORTAB_CDP_PORT`）。
- 新版本 Chrome 需要新符号/字段时，改 `resolver/spec.yaml`，不要在原生代码里硬编码偏移（同时看“偏移量”里的 schema 规则）。
- **失败即不改动**：偏移量、协议、身份任一校验不通过，原生功能整体关闭、标签栏保持原样，并通过 `status` 让设置页说明原因。不要为了“能用”放宽校验。
- 用户可见文案改动要同步 `extension/_locales/` 全部 8 种语言，`check_repo.py` 会检查键是否一致。
- 代码与注释用英文；文档用中文。
- 涉及标签栏坐标、真实鼠标拖拽的行为无法可靠自动化（见踩坑 13），改动后需人工在沙盒里验证一次；其余逻辑用 `e2e.py` 的测试钩子（`arbortab.__testDrop` 等）覆盖。

## 版本与兼容

扩展由商店自动更新，DLL 靠用户手动替换，两者必然长期错开。因此**兼容性只看契约号，从不要求两边版本号相同**。

- **发布版本**：唯一来源是根目录 `VERSION`。CMake 读取它生成 `ARBORTAB_VERSION` 和 DLL 版本资源；`extension/manifest.json` 的 `version` 必须与之相同（`check_repo.py` 检查）。发版时同时改这两处，再打 `vX.Y.Z` tag。
- **通信协议号**：`native/src/link/protocol.h` 的 `kProtocolMin/kProtocolMax` 与 `extension/src/background/native-link.js` 的 `PROTOCOL`。
  - 对已有消息做**不兼容**修改（改字段含义、删字段、改必需字段）：`kProtocolMax` +1，扩展的 `PROTOCOL` 随之 +1。
  - 只**新增**旧端可忽略的字段或消息：不升号。
  - 只有原生模块确定不再支持某个旧协议时，才提高 `kProtocolMin`。
  - 扩展握手时检查 `PROTOCOL ∈ [protocolMin, protocolMax]`；不在范围内就停止向 DLL 发送任何业务消息，设置页提示更新哪一边。
- **偏移量 schema**：`resolver/spec.yaml` 的 `schema` 与 `native/src/core/offsets.h` 的 `kOffsetsSchema`，两者必须相等。规则见下一节。

## 偏移量

- **生成**：`offsets.yml` 每 6 小时查询 stable 版本，PDB 上线后运行 resolver，发布到 `offsets` 分支。分支布局（由 `tools/ci/offsets_index.py` 维护，它也负责把旧布局迁移过来）：

  ```
  index.json                       {"format": 2, "schemas": {"<schema>": {"<chrome 版本>": "<pdb key>"}}}
  win64/s<schema>/<pdb key>.json
  ```

- **签名**：分支上每个文件都有同名 `.sig`（ECDSA P-256 对文件原始字节签名，base64 的 r‖s），由 `tools/ci/offsets_sign.py` 生成；公钥编在 `native/src/core/offsets_key.h`，私钥只在仓库 secret `OFFSETS_SIGNING_KEY` 里（维护者另有离线备份）。offsets 工作流每次运行都会给尚未签名的文件补签。**DLL 拒绝没有签名或签名不对的下载**（报 `error`），`config.ini` 的 `offsetsMirror` 可加一个优先镜像（测试、自建），同样要验签。换密钥 = 发新版 DLL 并重签全部文件。
- **撤回**：分支根目录的 `revoked.json`（同样签名）列出不得使用的文件的 SHA-256。按内容而不是按 key 撤回，所以修正后的表可以用同一个 key 重新发布。操作：手动运行 offsets 工作流并填 `revoke` 输入（文件的 sha256）。DLL 本地保存撤回表（`offsets\revoked.json`），启动时用已存的表检查缓存、后台每天最多刷新一次（新撤回下次启动生效）；下载前会先同步刷新。命中撤回的缓存会被删除并重新下载。
- **运行时**：DLL 按 `(kOffsetsSchema, pdbKey)` 查缓存 `%LOCALAPPDATA%\ArborTab\offsets\s<schema>\<key>.json`。没有缓存就依次从 GitHub raw、jsDelivr 下载，验签、查撤回、校验通过后原子写入缓存；都失败则每 30 分钟重试。同一个 PDB key 的二进制事实永不改变，所以**缓存永久有效、不做定期刷新**，只有撤回表会刷新。
- **加载校验**（`LoadOffsetsFromFile`）：`schema` 必须等于 `kOffsetsSchema`，`pdbKey` 必须等于当前 `chrome.dll` 的 key；不通过的缓存会被删除并重新下载。
- **什么时候必须升 schema**：DLL 开始依赖旧文件里没有的条目（新增 hook、字段、vtable 等），或文件格式变化。做法：
  1. `spec.yaml` 的 `schema` 与 `kOffsetsSchema` 同时 +1；
  2. 手动触发 offsets 工作流，为仍在使用的 Chrome 版本（当前 stable 及前两个）重新生成；新 schema 的文件到位之前，新 DLL 会显示“等待适配”；
  3. 旧 schema 的文件保留，供旧 DLL 继续使用。
  只给 spec 增加旧 DLL 用不到的条目时不必升号，但新条目对旧版本缺失，所以**新 DLL 不能依赖未升 schema 的新条目**。
- **状态上报**：偏移量管线把状态（`resolving`、`waiting`、`ready`、`disabled`、`error`）通过 `hello`/`status` 报给扩展；设置页据此区分“未安装”“等待适配”“校验失败”等情况。

## 通信协议

DLL 在 `chrome.dll` 映射后**立即**启动两个传输，不等偏移量，这样扩展才能区分“未安装”和“等待偏移量”。两者承载完全相同的 JSON 消息，由 `link/server.cpp` 统一处理。

- **主传输：Native Messaging 桥 + 命名管道**。
  - 扩展 `chrome.runtime.connectNative('com.arbortab.bridge')`，Chrome 启动 `arbortab-bridge.exe`（通常经 `cmd /c`），第一个参数是调用方 origin。宿主清单的 `allowed_origins` 决定哪些扩展能启动它。
  - 桥沿父进程链找到启动它的浏览器，连 `\\.\pipe\arbortab-<浏览器 PID>`，先发 `bridge.hello {origin, bridgeVersion}`，之后只转发字节（管道与 Native Messaging 的分帧相同：32 位小端长度 + JSON）。找不到管道就回一条 `bridge.unavailable` 后退出。
  - DLL 侧：管道用 `FILE_FLAG_FIRST_PIPE_INSTANCE` 防抢注，DACL 只允许当前用户和 SYSTEM，拒绝远程；连上后用 `GetNamedPipeClientProcessId` 确认对方的祖先进程里有本浏览器，否则断开。
  - 两端都用 overlapped I/O：同步句柄上一个线程挂着读时，另一个线程的写会被串行化卡住。
  - 每个浏览器实例只和自己的扩展配对，不占端口，Native Messaging 端口还能让 service worker 保持存活。
- **后备：WebSocket** `ws://127.0.0.1:47821–47825/arbortab`，给没有桥的旧安装和开发工具（`nativecheck.py`、`e2e.py` 的 `debug.layout`）用。服务端校验 `Origin: chrome-extension://<id>`。
- **扩展的选择顺序**（`native-link.js`）：先连桥；桥未注册（`missing`）、浏览器里没有模块（`module-not-loaded`）或其它失败时，本轮改试 WebSocket；都失败就退避重试（最长 30 秒）。当前传输和桥的状态会出现在设置页“关于与诊断”和调试信息里。
- **来源限制**：`config.ini` 的 `extensions` 可限定扩展 ID，两个传输都适用；为空时放行任意扩展（桥本身已受 `allowed_origins` 限制）。
- **握手**：扩展发 `hello {protocol, extVersion}`；DLL 回 `hello {protocolMin, protocolMax, nativeVersion, features, offsets {status, schema, chromeVersion}, safeMode {active, crashes, limit}}`。状态变化时（偏移量到位、功能启用）DLL 主动推送同样结构的 `status`。
- **业务消息**：扩展 → DLL：`state`（窗口标签顺序、每个标签的 depth/hidden/collapsed/hasChildren/hiddenCount、外观配置），DLL 只负责照着画、不保存树；`safeMode.reset`（用户在设置页点“重新启用”）。DLL → 扩展：折叠点击、`dragBegin`/`dropOnto`/`dragEnd`。
- **诊断**：`debug.report`（版本、构建、PID、传输、偏移量来源、安全模式、config、计数器、`native.log` 末尾 120 行；协议不兼容时也允许）、`debug.layout`（布局快照，测试用）。

## 安全模式（崩溃保护）

`core/crash_guard`：hook 装上前在 `%LOCALAPPDATA%\ArborTab\sessions\<pid>.session` 写会话文件，正常退出（`DLL_PROCESS_DETACH` 且进程在退出）时删除并清零计数。下次启动时，进程已不存在的会话文件算一次异常退出（被强杀也算，与 Chrome 自己的判断一致）。同一 `(模块版本, chrome.dll key)` **连续 2 次**后不再安装 hook，状态里 `safeMode.active=true`，设置页显示“已暂停”和“重新启用”按钮（立即重装 hook，不用重启）。换 Chrome 版本或模块版本自动从零计。`config.ini` 的 `safeMode=0` 可关闭。
- 开发时 `sandbox.py kill`（`dev.ps1` 会调用）会删掉被杀进程的会话文件，否则两次重启就会进安全模式；要测安全模式用 `kill --as-crash`。

## 发布与分发

- **流程**：改 `VERSION` 与 manifest → 合入 main（`ci.yml` 通过）→ 推 `vX.Y.Z` tag → `release.yml` 构建并创建**草稿** Release（扩展 zip、原生 zip、单独的 install.ps1/uninstall.ps1、SHA256SUMS）→ 在沙盒里人工验证真实拖拽 → 发布草稿。
- **前提**：仓库 secret `OFFSETS_SIGNING_KEY` 已设置（没有它 offsets 工作流会失败，DLL 也拿不到可用的偏移量）。
- **用户安装方式**：设置页（未连接或需要更新时）给出一行命令，README 里也有同一行：

  ```
  powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"
  ```

  - 因此每个 Release 除了 zip，还必须单独上传 `install.ps1`、`uninstall.ps1` 作为资产（`package.py` 已处理）。**资产名不要改**，否则已发出去的命令会失效。
  - 安全取舍只在设置页、README 这些给出命令的地方说明，运行命令即视为同意；脚本里不再询问。
  - `install.ps1` 发现身边没有 `version.dll`（即一行命令模式），就通过 GitHub API 下载最新 Release 的 `arbortab-native-*-win64.zip`，解压后运行包内的 `install.ps1`。提权重启必须基于文件，所以要先落盘再运行；`uninstall.ps1` 同理，会先把自己下载到临时目录。
  - 在 `irm | iex` 下**绝不能调用 `exit`**（会关掉用户的控制台），只能 `return`/`throw`；Windows PowerShell 5.1 要显式启用 TLS 1.2 才能访问 GitHub。
  - 测试时用环境变量 `ARBORTAB_PACKAGE`（本地 zip，代替下载）和 `ARBORTAB_CHROME_DIR`（目标目录，如沙盒）。`iex` 无法传参，便携版 Chrome 也只能用 `ARBORTAB_CHROME_DIR` 指定，**测试时务必设置它，否则会装进本机 Chrome**。
- **原生包**：`version.dll` + `arbortab-bridge.exe` + `com.arbortab.bridge.json` + `install.cmd`/`install.ps1` + `uninstall.cmd`/`uninstall.ps1` + LICENSE，供想先检查文件再安装的用户使用。安装脚本会：
  - 找到系统级和用户级 Chrome，并要求 Chrome 已退出；
  - 在**提权之前**以当前用户身份装桥：复制到 `%LOCALAPPDATA%\ArborTab\bridge\`，并在 `HKCU\Software\Google\Chrome\NativeMessagingHosts\com.arbortab.bridge` 注册（提权后的重跑带 `-SkipBridge`，因为 HKCU 属于原用户）；卸载同理，先注销再删除；
  - 只在目录不可写时才请求管理员权限；
  - 拒绝覆盖不是 ArborTab 的 `version.dll`，也不安装不是 ArborTab 的桥（按版本资源的 ProductName 判断，两个二进制共用 `version.rc.in`）；
  - 把安装位置记录到 `%LOCALAPPDATA%\ArborTab\install.json`，供卸载使用。
- **扩展包**：**保留** manifest 的 `key`，这样解压加载的 Release 与开发版 ID 相同（`egjpjafmlomkpnncliaigklbdbflface`），正好是桥的 `allowed_origins`；只去掉测试和签名材料。上架商店后会得到另一个 ID，届时把它加进 `com.arbortab.bridge.json` 的 `allowed_origins`。`check_repo.py` 会核对 key 推出的 ID、宿主清单、`native-link.js` 的宿主名和 `dev.py` 的 `EXTENSION_ID` 一致。
- **后续计划**（尚未实现）：上架商店（并把商店 ID 加入 `allowed_origins`）；DLL 与桥的代码签名；商店自动上传；extended-stable 等更多渠道；确认所有用户都有桥后移除 WebSocket 后备。实测待办：Chrome 自动更新后 `Application\version.dll` 是否保留；SmartScreen/Defender 对未签名 DLL 与桥的反应。

## 环境

- Windows x64，VS 2022 Build Tools（MSVC，`vcvars64.bat`）、Windows SDK、cmake + Ninja、uv、node；崩溃分析要用 Windows SDK 的 Debugging Tools（cdb）。
- **本机信息**（沙盒路径、本地 PDB 缓存、工具链的具体位置和版本）写在根目录的 `AGENTS.md.local`。它被 gitignore、不提交，`CLAUDE.md` 会一并引用；换机器时参照本节自建一份。
- 当前对照版本：Chrome 稳定版 156.0.8078.80，PDB key `B2C7C3B72F30D3ED4C4C44205044422E1`。该版本全部 RVA/偏移见 `offsets/win64/s2/B2C7C3B72F30D3ED4C4C44205044422E1.json`。

### Chromium 源码取用

浅克隆 + sparse checkout（远端 `https://github.com/chromium/chromium.git`）：

```sh
git fetch --depth 1 --filter=blob:none origin refs/tags/<version>
git sparse-checkout set --no-cone <目录...>
```

值得取的目录：`chrome/browser/ui/views/tabs/`、`chrome/browser/ui/tabs/`、`chrome/browser/ui/views/frame/`、`components/tabs/`、`ui/views/layout/`、`components/sessions/content/`。

关键文件：

- `.../tabs/common/unpinned_tab_container_view_layout.cc`：垂直布局，含“不可见子项高度为 0”的分支
- `.../common/tab_view.{h,cc}`：`OnMousePressed`、`GetTabInterface`、`icon_`、`collapsed_`
- `.../common/tab_view_vertical_layout.cc`：标签内部布局
- `.../common/dragged_tabs_container.cc`：`HandleTabDragInContainer`、`GetTargetForTabDrag`；原生拖拽是“跨过中线即重排”的实时重排
- `.../common/tab_drag_handler.{h,cc}`：`InitializeDrag`、`HandleDraggedTabsIntoPosition`、`EndDrag`
- `ui/views/layout/proposed_layout.h`：`ChildLayoutList` = `absl::InlinedVector`
- `components/sessions/content/session_tab_helper.h`：`IdForTab`

垂直标签的偏好设置键是 `vertical_tabs.enabled`（在 Preferences 里）。

### 逆向小工具（`tools/re/`）

- `pdbinfo.py`：读 chrome.dll 的 PDB key。`uv run --with pefile python -I tools/re/pdbinfo.py <chrome.dll>`
- `symq.py`：dbghelp 查询。`sym <通配名>` 查符号（设 `RAW=1` 时按修饰名查）、`type <类型名>` 查字段偏移。`uv run python -I tools/re/symq.py <pdb> sym|type ...`
- `dis.py`：查看指定 RVA 的机器指令。`uv run --with pefile --with capstone python -I tools/re/dis.py <dll> 0xRVA:len`

## 已实测的事实

- `chrome.exe` 的导入表含 `VERSION.dll`（`GetFileVersionInfoSizeW`、`GetFileVersionInfoW`、`VerQueryValueW`），因此同目录的 `version.dll` 会被加载进浏览器进程。
- 在 `chrome.dll` 的加载完成通知（`LdrRegisterDllNotification`）里安装 MinHook 接管可行；网络、下载这类重操作必须挪到后台线程。
- 扩展连接 `ws://127.0.0.1` 时，manifest 声明了 `host_permissions` 就**没有权限提示**；服务端收到 `Origin: chrome-extension://<id>`，据此做来源校验。
- Chrome 符号服务器上有正式版 `chrome.dll` 的完整私有 PDB，含类型信息。
- PDB key = PE 调试目录里 CodeView 记录的 GUID（32 位十六进制）+ Age（十六进制），无分隔符。
- 符号服务器：`https://chromium-browser-symsrv.commondatastorage.googleapis.com/chrome.dll.pdb/<key>/chrome.dll.pdb`，gzip 传输编码（约 1.08GB，**curl 要加 `--compressed`**），解压后约 5.7GB。新版本发布后 PDB 常要数小时甚至数天才上线。

## 踩坑记录

1. **系统 `version.dll` 必须在 `DllMain(DLL_PROCESS_ATTACH)` 里立即加载**。子进程进入沙箱后再加载会失败，之后调用就是空指针崩溃。
2. **`views::ProposedLayout::child_layouts` 是 `absl::InlinedVector<ChildLayout, 8>`，不是 `std::vector`**，按 `std::vector` 读会崩溃。
3. **bash heredoc 写 C 字符串会被错误转义**：`L"\\version.dll"` 变成 `L"\version.dll"`。生成文件后要检查。
4. **未签名的 `version.dll` 会让所有子进程创建失败**：Chrome 对渲染/GPU/ServiceWithJit 等子进程设置 `MITIGATION_FORCE_MS_SIGNED_BINS`（即 `PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON`），只放行 MS 签名 DLL 与 `chrome.dll`/`chrome_elf.dll`。后果是 `CreateProcessAsUserW` 返回 `ERROR_INVALID_PARAMETER`，页面全部 `Aw, Snap! (STATUS_INVALID_IMAGE_HASH)`，一个页面都打不开。
   - 做法（`native/src/core/cig.cpp`）：在浏览器进程里接管 `kernelbase!UpdateProcThreadAttribute`，只在 `Attribute == PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` 时把这一位清掉。
   - 只清这一位；不要顺手清 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`（131074）等其它属性，否则同样返回 `ERROR_INVALID_PARAMETER`。
   - 安全影响：这等于降低了渲染进程的代码完整性防护，属于必须向用户说明的取舍。
5. **ICF 同址折叠**：`DraggedTabsContainer::OnTabDragEnded` 与 `OnTabDragExited` 在 156 中共用同一地址，不能作为接管点；`tabs::TabModel::GetContents` 也与 `v8::internal::LargeObjectSpace::Size` 折叠，但它只被调用、不被接管，属于正常现象。用 `SymEnumSymbolsForAddr` 检查同址情况，只对真正要接管的函数做同址检查。
6. **静态存储期的 `std::thread` 会让 DLL 卸载时 abort**（0xC0000409，栈落在 `VERSION!abort`）：卸载时 atexit 析构器发现 `std::thread` 仍 joinable → `std::terminate`。WebSocket 的 accept 线程必须 `detach()`。
7. **布局刷新不能同步调 `LayoutManagerBase::Layout`**：在消息回调里同步跑布局会重入并进入重布局风暴。正确做法是 `InvalidateLayout(host_view)` + `InvalidateManager(layout)` + `SchedulePaint(host_view)`，让 Chrome 自己的帧跑一次布局。
   - `CalculateProposedLayout` 的 `this` 是 **LayoutManagerBase，不是 view**；要刷新的容器 view 在 `views::LayoutManagerBase::host_view_`。拿错指针调 `InvalidateLayout` 会直接搞崩浏览器。
   - 记录过的 host view 随时可能被销毁，刷新前要校验 vtable、设 TTL（60s），并用 `__try/__except` 包住每次调用。SEH 不能和需要 C++ 对象展开的函数混用，必须拆成独立函数。
8. **`chrome.tabs.onCreated` 的 payload 不含 `openerTabId`**（156 实测，虽然文档列出该字段）。必须再 `chrome.tabs.get(tab.id)` 读一次才能拿到 opener。
9. **窗口的最后一个标签关掉后 Chrome 正常退出（exit code 0）**，测试要留一个无关标签，否则会把“关完子树后浏览器消失”误判成崩溃。
10. **消息专用窗口只能由 UI 线程创建**：`UiThreadEnsureWindow()` 若被偏移量解析线程（或任何非 UI 线程）先调用，窗口就挂在没有消息循环的线程上，之后所有 `PostMessage` 刷新都静默失效（表现为“缩进/折叠不生效”）。只从布局接管入口创建，并用原子标志防止并发创建。
11. **`TabView::OnMousePressed` 的命中测试坐标不能相加**：`ui::LocatedEvent::location_` 已经是 TabView 局部坐标，`icon_->bounds_` 也是相对父视图的，两者相加会整体偏移一个标签原点，永远点不中。
12. **调用 `gfx::Canvas::DrawStringRect` 等文字 API 时注意 ABI**：`std::u16string_view` 按指针传 `{ptr, len}` 结构，`FontList` 取 `views::Label::GetDefaultFontList()`（或其 `DeriveWithSizeDelta`）。这些签名跨版本容易变，出错时会被 SEH 静默吞掉——文字不出现时先怀疑签名，再核对 PDB。
13. **自动化点击浏览器 UI 不可靠**：本机终端与沙盒窗口共享桌面，`PostMessage`/`SendInput` 都很难稳定命中标签栏，也起不了标签拖拽。涉及标签栏坐标、拖拽的验证只能人工做。
14. **读 `host_view_` 的 `views::View::children_` 拿不到子 view**（读到空集合）。落点判定改用布局自己产出的快照：上一轮布局里每行可见 TabView 的矩形（容器坐标，已含缩进与隐藏处理）。
15. **标签条内部不要调用原生拖拽 handler**：它在每次移动时都改 tab model，触发原生“其他标签让位”的实时重排，接管压不住。
16. **快捷键：Chrome 会拒绝部分字母组合**（如 `Alt+Shift+KeyW`），可用的组合是 `Alt+Shift+Comma` / `Alt+Shift+Up` / `Alt+Shift+Down`。
17. **沙盒重启后扩展可能还在跑旧代码**：版本号不变时，Chrome 会沿用缓存的 service worker 脚本，`Extensions.loadUnpacked` 同一路径也不会刷新。改了扩展代码后，先在 service worker 里执行 `chrome.runtime.reload()` 再测；否则测试可能对着旧代码通过。
18. **spec 加了 DLL 依赖的条目却没升 schema，旧文件会被“合法地”接受**：schema 1 时期发布的 156 文件缺少后来加的字段，新 DLL 照样加载，结果 layout/twisty 静默关闭；本机缓存是新生成的，开发时完全看不出来。已因此升到 schema 2。改 spec 时按“偏移量”一节判断是否升号，并用一次真实下载（删掉本地缓存）验证。
19. **签名要覆盖 git 存储的字节**：Windows runner 默认 `core.autocrlf=true`，签的是 CRLF 的工作区副本，GitHub 发的却是 LF，于是全部验签失败。offsets 工作流已关掉 autocrlf，分支上有 `* -text`；在本机操作 offsets 分支时同样用 `git -c core.autocrlf=false`。另外 raw.githubusercontent.com 有约 5 分钟缓存，刚推送的文件要等一会儿才能下到。
20. **CDP `Extensions.loadUnpacked` 加载的扩展不会跨重启保留**：不经 `dev.ps1` 直接重启沙盒（比如测安装脚本后），浏览器里根本没有扩展，看起来像“连不上”。这种测试要在启动后再加载一次扩展。

## 符号与偏移核对经验

- 用 dbghelp：`SymLoadModuleExW` 直接加载 `.pdb`、`SymEnumSymbolsW`、`SymEnumTypesByNameW` + `SymGetTypeInfo`。加载 5.7GB 的 PDB 约 50 秒。
- **重载函数用修饰名（mangled name）精确匹配**（关闭 `SYMOPT_UNDNAME` 后枚举 public 符号），例如 `?FillRect@Canvas@gfx@@QEAAXAEBVRect@2@I@Z`。
- 部分函数只有私有符号、没有 public 符号（如 `TabView::GetTabInterface`、`DraggedTabsContainer::HandleTabDragInContainer`）。此时用去修饰名匹配，并要求结果唯一（`spec.yaml` 里写 `private:`）。
- prologue 从 chrome.dll 文件里读（RVA → 文件偏移用 pefile），运行时先比对前若干字节再启用功能。
- 拿到 RVA 后要回到机器指令上核对行为，例如 `UnpinnedTabContainerViewLayout::orientation_` 非 0 时才会走 `CalculateVerticalLayout`——这类判断靠符号表看不出来。

## 运行时数据访问经验

- **成员函数返回结构体的调用约定**（MSVC x64 / clang-cl）：`rcx`=this，`rdx`=返回值缓冲区，`r8` 起才是参数，返回 `rax`=返回值缓冲区。
- **`views::ProposedLayout`**（156：size 0x190）：`+0x00 host_size`（`gfx::Size`）、`+0x08 child_layouts`（`absl::InlinedVector<ChildLayout, 8>`）：
  - `+0x08` metadata = `(size << 1) | is_allocated`
  - `+0x10` `is_allocated == 0` 时是内联数组起点；否则是 `{ ChildLayout* data; size_t capacity; }`
- **`views::ChildLayout`**（size 0x30）：`+0x00 child_view (View*)`、`+0x08 visible (bool)`、`+0x0c bounds (gfx::Rect)`、`+0x1c available_size`。
- Chromium 用 libc++（`std::__Cr`），**不要在接口上传递 MSVC STL 的 `std::string`/`std::vector`**；字符串只用 `{ptr, len}` 结构。
- **识别 TabView**：读 view 对象首 8 字节（vtable 指针），与 PDB 里 `TabView` 的各 vtable 地址比较。
- **取 tabId**：`TabView::GetTabInterface(view)` → `TabInterface*` → 虚函数 `GetContents()`（槽位号由 vtable 反查）→ `sessions::SessionTabHelper::IdForTab(out, contents)`。`IdForTab` 是静态函数，通过隐藏指针返回：`rcx` = 输出 `SessionID*`（4 字节 int），`rdx` = `const WebContents*`，得到的值就是扩展里的 `tab.id`。
- 隐藏标签用“`visible=false` + `bounds.height=0`”表达即可：原生实现里 `IsChildVisibleInContainer` 返回 false 时本来就不占位。
- 不要改标签标题文本：标题是 libc++ 布局的字符串，且会被页面标题更新覆盖。

## 开发与测试方法

- **沙盒 Chrome**（`tools/dev/sandbox.py`、`dev.py` 已封装）：把 `Application\chrome.exe` 与 `<version>\` 目录复制到独立目录（约 500MB），放入开发中的 `version.dll`，启动参数：

  ```
  --user-data-dir=<临时目录> --no-first-run --no-default-browser-check
  --remote-debugging-port=9334 --enable-unsafe-extension-debugging
  ```

  预先在 `Default\Preferences` 写入 `{"vertical_tabs":{"enabled":true,"collapsed_state":false,"uncollapsed_width":240}}`，启动后直接显示垂直标签。
- **加载扩展**：通过 CDP 浏览器级 WebSocket 发送 `Extensions.loadUnpacked {path}`。Chrome 137+ 正式版已不支持 `--load-extension`。
- **用户侧诊断**：扩展在 service worker 内存里保留最近 200 条事件（`diagnostics.js`：连接、断开、hello/status、消息处理失败），设置页“关于与诊断”可复制或下载调试信息：扩展侧（版本、连接方式与偏移量状态、设置、树的规模、事件）加上原生侧的 `debug.report`（含 `native.log` 末尾 120 行），**不含网址和标题**。新增事件或字段时保持这一点。`native.log` 超过 1MB 轮转，保留 3 份旧文件。
- **自动化断言**：CDP 附加到扩展 service worker，调用 `arbortab.getState()` 读树（`windows[id] = {order, parents, collapsed}`）；原生侧用 `debug.layout` 取布局快照，回复里的 `stats.dropTarget` / `stats.dropZone` 可直接断言落点，不必截图。
- **截图核对**：`sandbox.py shot <png>` 按窗口矩形截图，人工核对。
- **崩溃分析**：沙盒配置目录下 `Crashpad\reports\*.dmp` 用 `cdb -z <dmp> -c ".ecxr; r; kn"` 查看；旧版 cdb 读不了新 PDB，地址可用 `symq.py` 反查。事件查看器的 Application 日志里也有崩溃记录。
- **WebSocket 报文**：单帧文本即可（状态 JSON 通常 < 64KB），但仍要支持扩展长度字段。

## 树模型（扩展侧）

- 顺序 = 树的前序遍历，不单独存顺序，所以永远自洽、不需要 `normalize()` 之类的修复。
- 结构操作只有两种，都由扩展执行：改父（`attachAsLastChild`）、换兄弟位置（`insertSibling`，父节点不变）。
- **不要**让 Chrome 自由重排后再从落点索引反推父节点——这是旧方案树错乱的来源。`tabs.onMoved` 只重算兄弟顺序（`adoptOrder`）、父关系不动，再用 `tabs.move` 把前序顺序收敛回来。
- **被拖的是哪个标签**由 `TabView::OnMousePressed` 记录，随 `dragBegin` / `dropOnto` 一起传递；不要用“当前激活标签”去猜。
- 拖拽期间临时折叠被拖的展开父标签，`dragEnd` 后恢复；`dropOnto` 拒绝把父节点放进自己的后代。

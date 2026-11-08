# ArborTab 开发指南

ArborTab 让 Chrome 原生的**垂直标签栏**变成树状标签（Windows x64）。由两部分组成：

- **扩展**（`extension/`，MV3）：维护标签树（父子、折叠），执行所有结构操作（`chrome.tabs.move` 等），并把显示状态推给原生模块。
- **原生模块**（`native/`，`version.dll` 代理）：放在 `chrome.exe` 同目录，被浏览器进程加载后用 MinHook 接管标签栏的布局、绘制、鼠标和拖拽，实现缩进、隐藏、折叠标记、拖放落点。通过 `ws://127.0.0.1:47821-47825/arbortab` 与扩展通信。
- **偏移量**：原生模块所需的函数 RVA、字段偏移、vtable 等按 Chrome 版本由 `resolver/` 从官方 PDB 生成，CI 发布到 `offsets` 分支，运行时按 PDB key 下载。

## 目录结构

```
extension/          MV3 扩展；src/background/tree.js 是纯树模型，test/ 是它的单元测试
native/             version.dll（CMake + MSVC）；src/core 基础设施、src/features 各功能、src/link WebSocket
  third_party/      vendored MinHook、nlohmann/json
resolver/           resolve.py + spec.yaml：从 chrome.dll + PDB 生成偏移量 JSON（缺任何一项即失败）
offsets/            已发布偏移量的本地副本（运行时实际从 GitHub offsets 分支下载）：index.json（版本 → PDB key）、win64/<key>.json
tools/
  ci/               CI 用：query-chrome.py（Omaha 查稳定版）、extract-chrome.ps1（解包安装包）
  dev/              本地开发：build-native.ps1、dev.py/dev.ps1（一键重建+重启沙盒）、sandbox.py、nativecheck.py
  re/               逆向/符号：pdbinfo.py、symq.py、dis.py
  test/             e2e.py（沙盒 Chrome 中的端到端检查）
.github/workflows/  offsets.yml：每 6 小时检查新稳定版并发布偏移量
artifacts/ build/   本地产物（已 gitignore）
```

## 常用命令

```sh
# 构建原生模块 → artifacts/native/version.dll
pwsh -File tools/dev/build-native.ps1 [-Configuration Debug]

# 一键开发循环：构建 → 停沙盒 → 装 DLL → 启动 → 加载扩展 → 从 native.log 核对正在运行的构建
pwsh -File tools/dev/dev.ps1 [-NoBuild]

# 树模型单元测试
node --test extension/test/tree.test.mjs

# 端到端（需沙盒 Chrome 已运行）
uv run --with websocket-client python tools/test/e2e.py tree|drop|tree-visual

# 直接驱动原生端点（不经扩展）/ 查看布局快照
uv run --with websocket-client python tools/dev/nativecheck.py --layout

# 生成偏移量
uv run --with pefile --with pyyaml python resolver/resolve.py --dll <chrome.dll> --pdb <chrome.dll.pdb> --out <key>.json
```

原生模块日志：`%LOCALAPPDATA%\ArborTab\logs\native.log`。

## 工作约定

- **只动沙盒 Chrome，不碰本机安装的 Chrome**。沙盒目录默认 `C:\Users\evative7\Desktop\Playground\_test\portable`（`ARBORTAB_SANDBOX` 可改），CDP 端口 9334（`ARBORTAB_CDP_PORT`）。
- 新版本 Chrome 需要新符号/字段时，改 `resolver/spec.yaml`，不要在原生代码里硬编码偏移。
- 代码与注释用英文；文档用中文。
- 涉及标签栏坐标、真实鼠标拖拽的行为无法可靠自动化（见踩坑 13），改动后需人工在沙盒里验证一次；其余逻辑用 `e2e.py` 的测试钩子（`arbortab.__testDrop` 等）覆盖。

## 环境

- Windows x64，VS 2022 Build Tools（MSVC 14.44，`vcvars64.bat`）、Windows SDK 10.0.26100、cmake + Ninja、uv、node。
- cdb：`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`
- 本地 PDB 缓存（Chrome 156，5.7GB）：`C:\Users\evative7\Desktop\Playground\_dl\pdb\chrome.dll.pdb`
- 当前对照版本：Chrome 稳定版 156.0.8078.80，PDB key `B2C7C3B72F30D3ED4C4C44205044422E1`。该版本全部 RVA/偏移见 `offsets/win64/B2C7C3B72F30D3ED4C4C44205044422E1.json`。

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

# ArborTab 实施计划

> 仓库名：`chrome-plugin-arbortab`　许可证：GPL-3.0
> 目标平台：Windows x64 + Google Chrome 稳定版（当前 156/157），已启用原生垂直标签页。

## 0. 背景与目标

Chrome 从 146 起内置垂直标签页，但只是一个平铺列表。ArborTab 在**保留原生垂直标签栏**的前提下，给它加上 Firefox Tree Style Tab 式的树形管理：

- 从某个标签打开的新标签自动成为它的子标签（缩进显示）
- 折叠 / 展开子树
- 关闭父标签时关闭整棵子树
- 拖拽调整层级：把标签拖到另一个标签中部松手，它就成为该标签的子标签

不需要的功能：重启后恢复树结构（明确不做）。

### 设计原则

1. **逻辑与显示分离**：树的全部逻辑放在 Chrome 扩展里（JS，只用官方 API）；原生模块只负责“显示层”的调整（缩进、隐藏、高亮、捕获少量鼠标事件），**从不修改 Chrome 的标签数据模型**，所有对标签的真实操作（移动、关闭、选中）都通过扩展 API 完成。
2. **失败即原样**：原生模块无法确认当前 Chrome 版本的符号信息时，什么都不做，Chrome 保持原样；单个功能校验失败只关闭该功能。
3. **版本适配自动化**：Chrome 每次更新后，由 CI 从 Google 公开符号服务器取得该版本的调试符号（PDB），自动生成函数地址与结构体字段偏移表（offsets），发布到仓库；各台电脑的原生模块按需下载这个几 KB 的文件。

## 1. 总体架构

```
┌──────────────────────────── Chrome 浏览器进程 ────────────────────────────┐
│                                                                          │
│  ArborTab 扩展 (MV3 service worker)          原生模块 (version.dll 代理)   │
│  ─ 树模型 / 配置(storage.sync)                ─ 读取 offsets 表              │
│  ─ tabs.* 事件处理                ◀──WS──▶    ─ 拦截少量 Views 函数：         │
│  ─ 推送每个标签的显示状态                        布局后处理 / 鼠标 / 绘制 / 拖拽 │
│                                  127.0.0.1    ─ 回报 UI 事件给扩展           │
└──────────────────────────────────────────────────────────────────────────┘
            ▲                                         ▲
            │ Chrome 应用商店（公开发布，随账号同步）     │ offsets/<PDB GUID>.json
            │                                         │ (GitHub raw，按需下载并缓存)
                                    GitHub Actions：每小时检查新版本 → 下载 PDB → 生成 offsets
```

三个产物：

| 产物 | 语言 | 发布位置 | 变更频率 |
|---|---|---|---|
| 扩展 ArborTab | JS（MV3） | Chrome 应用商店（公开）+ Releases 里的 zip | 加功能时 |
| 原生模块 `version.dll` | C++（MSVC） | GitHub Releases | 很少 |
| offsets 表 | JSON | 仓库 `offsets` 分支 | 每个 Chrome 版本自动生成 |

## 2. 仓库结构（建议）

```
chrome-plugin-arbortab/
├─ PLAN.md
├─ extension/                 # MV3 扩展
│  ├─ manifest.json
│  ├─ src/background/         # service worker：tree.js, native-link.js, handlers.js
│  ├─ src/options/            # 设置页
│  └─ test/                   # 树逻辑单元测试（纯 JS，可在 node 下跑）
├─ native/                    # 原生模块
│  ├─ CMakeLists.txt (或 build.bat)
│  ├─ src/proxy/              # version.dll 导出转发
│  ├─ src/core/               # offsets 加载、模块定位、功能开关、日志
│  ├─ src/features/           # layout / mouse / paint / drag
│  ├─ src/link/               # WebSocket 服务端 + JSON
│  └─ third_party/minhook/    # MinHook（BSD-2），以 git submodule 或拷贝方式引入
├─ resolver/                  # Python：PDB → offsets JSON（CI 与本地共用）
│  ├─ resolve.py
│  ├─ spec.yaml               # 需要解析的符号/类型/字段清单
│  └─ dbghelp_query.py        # 基于 research/tools/symq.py 整理
├─ scripts/
│  ├─ install.ps1 / uninstall.ps1
│  ├─ fetch-chrome.py         # Omaha 查询 + 下载 + 解包 chrome.dll
│  └─ dev-sandbox.ps1         # 复制一份 Chrome 到沙盒目录运行（开发测试用）
├─ .github/workflows/
│  ├─ build.yml               # 构建 DLL 与扩展 zip，发布 Release
│  └─ offsets.yml             # 每小时：新版本 → 生成 offsets → 推到 offsets 分支
└─ research/                  # 前期调研产物（见 §13），只作参考
```

## 3. 交互规格（最终版）

| 场景 | 行为 |
|---|---|
| 从当前页打开新标签（中键、Ctrl+点击、`target=_blank`、页面 JS `window.open` 打开的新标签） | 成为来源标签的子标签，放在来源标签已有子树的末尾。判定依据：`tabs.onCreated` 的 `openerTabId` |
| Ctrl+T / 新建标签按钮 / 书签 / 地址栏 / 其他扩展打开 | 没有 opener，作为顶层标签 |
| 弹出为独立小窗口（popup 类型窗口） | 不在标签栏中，忽略 |
| 点击**有子标签的标签的图标（favicon）** | 折叠 / 展开它的子树；不切换激活标签 |
| 折叠状态 | 子孙标签在标签栏中隐藏；父标签显示折叠标记和隐藏数量（如 `+3`） |
| 切换到被隐藏的标签（Ctrl+Tab、搜索标签、扩展 API 等） | 自动展开它的所有祖先 |
| 关闭父标签（任何方式：×、Ctrl+W、中键） | 关闭整棵子树。可配置：子孙数 ≥ N 时先确认，**默认不确认** |
| 拖拽：标签拖到另一个标签的**中部区域**（上下各留 1/4 走原生逻辑） | 目标标签高亮；松手后被拖标签（连同其子树）成为目标的最后一个子标签 |
| 拖拽：拖到上 1/4 或下 1/4 | 原生排序；落点后由扩展推断新父节点：成为落点下方相邻标签的兄弟（取下方标签的父节点）；落在末尾则取上方标签的父节点 |
| 拖动一个展开的父标签 | 拖动期间其子孙临时隐藏（视觉上只拖一个），松手后扩展把子树整体移到父标签之后 |
| 把标签拖出到新窗口 / 其他窗口 | 子树一起移动到目标窗口 |
| 快捷键（`commands`，用户可在 chrome://extensions/shortcuts 改） | 折叠/展开当前树、关闭当前树、提升一级（outdent）、降低一级（indent，成为上一个兄弟的子标签） |

范围约定：

- **树形只作用于“未固定、不在标签组里”的标签**。固定标签与组内标签按 depth 0 处理，不隐藏、不缩进；有 opener 的新标签若被 Chrome 自动放入组内，则不纳入树。
- 垂直标签栏处于**收起（只显示图标）**状态时，不缩进，但折叠隐藏仍生效。
- 不考虑 RTL 布局、横向标签栏（横向时原生模块不做任何事）。

## 4. 组件 A：扩展（MV3）

### 4.1 manifest 要点

- `permissions`: `tabs`, `storage`
- `host_permissions`: `ws://127.0.0.1/*`, `http://127.0.0.1/*`（**必须**，见 §13.3，有它才不会出现本地网络访问权限提示）
- `background.service_worker`
- `commands`：§3 中的 4 个快捷键（Chrome 限制最多 4 个有默认键位）
- `options_page`
- 固定 `key` 字段，使本地开发版与商店版扩展 ID 一致（原生模块按 ID 做来源校验）
- 名称中不出现 “Chrome”（商店商标规则）

### 4.2 树模型

```js
// 每个窗口一棵森林；节点按真实标签顺序排列
nodes: Map<tabId, { parentId: number|null, collapsed: boolean }>
```

- 真实标签顺序以 `chrome.tabs.query({windowId})` 的 index 为准；树必须满足“**每个子树在真实顺序中连续，且紧跟在父节点之后**”。任何事件处理后都执行一次 `normalize(windowId)`：检测违例并用 `tabs.move` 修复（批量、去抖）。
- 状态保存在 `chrome.storage.session`（service worker 重启后恢复；浏览器重启不恢复，符合需求）。
- 配置保存在 `chrome.storage.sync`：缩进宽度(px，默认 16)、关闭确认阈值(默认 0=不确认)、新建标签按钮打开的标签是否作为当前标签的子标签(默认否)、是否显示折叠数量(默认是)。

### 4.3 事件处理

| 事件 | 处理 |
|---|---|
| `tabs.onCreated` | 有 `openerTabId` 且 opener 在同窗口、两者都满足 §3 范围约定 → 设为子标签；必要时移到 opener 子树末尾 |
| `tabs.onRemoved` | 若被关的是父节点 → 关闭全部子孙（按阈值确认）；`removeInfo.isWindowClosing` 时忽略 |
| `tabs.onMoved` | 拖拽进行中（收到 `dragBegin` 未收到 `dragEnd`）时只记录，不处理；否则按 §3 规则推断父节点并 normalize |
| `tabs.onActivated` | 若目标被折叠隐藏 → 展开祖先 |
| `tabs.onAttached/onDetached` | 子树跟随移动到新窗口 |
| `tabs.onUpdated`(pinned / groupId 变化) | 移出树（其子标签提升一级） |
| 原生事件 `twistyClick` | 切换折叠 |
| 原生事件 `dropOnto` | 以当前窗口中被选中（highlighted）的标签为被拖集合，连同子树移到目标子树末尾，设为目标的子标签 |
| 原生事件 `dragBegin/dragEnd` | 暂停/恢复 onMoved 处理；dragEnd 后统一 normalize 并恢复临时隐藏 |

确认对话：service worker 不能弹窗，用 `chrome.windows.create({type:'popup'})` 打开一个小确认页，或者在父标签角标上闪烁提示后需二次关闭。具体选简单的一种即可。

### 4.4 与原生模块的连接

- `native-link.js`：连接 `ws://127.0.0.1:<port>/arbortab`，端口默认 `47821`，失败依次尝试 `47822..47825`；断线指数退避重连。
- 每 20 秒发送心跳（保持 MV3 service worker 存活，Chrome 116+ 的 WebSocket 活动会延长 SW 生命周期）。
- 任何树变化后，去抖 16ms，发送受影响窗口的**完整显示状态**（见 §6）。
- 原生模块不可用时（未安装 / 版本未适配）：树逻辑照常工作（关闭子树、快捷键等），只是没有缩进和折叠显示；折叠功能此时禁用（否则隐藏的标签用户看不到又找不回）。设置页显示原生模块状态。

### 4.5 测试

- 树逻辑写成不依赖 `chrome.*` 的纯函数模块，用 node 单元测试覆盖：创建/关闭/移动/拖入/折叠/跨窗口/固定/分组。
- 集成测试见 §10。

## 5. 组件 B：原生模块

### 5.1 加载方式

- 以 `version.dll` 名义放在 `chrome.exe` 同目录（chrome.exe 静态导入 `VERSION.dll`，已验证会加载同目录文件）。
- **导出转发**：实现系统 `version.dll` 的全部 17 个导出（列表见 `research/poc/poc.def`），转发到 `%SystemRoot%\System32\version.dll`。
  - **必须在 `DllMain(DLL_PROCESS_ATTACH)` 中立即加载系统 version.dll**。子进程进入沙箱后再加载会失败，导致调用空指针崩溃（已踩坑）。
  - 转发实现可以用 PoC 中的“通用 8 参数转发”写法，或参考 chrome_plus 的 `hijack.cc` 做法（也是 GPL-3.0，可复用）。
- **只在浏览器主进程工作**：命令行不含 `--type=` 才继续；其他进程只做导出转发。
- 等待 `chrome.dll`：用 `LdrRegisterDllNotification`，在 chrome.dll 映射完成的通知里读取模块基址。PoC 中在该回调内直接安装函数拦截可行；网络、文件下载等重操作一律放到后台线程。

### 5.2 版本识别与 offsets 获取

1. 从内存中 chrome.dll 的 PE 调试目录读取 CodeView 记录：PDB GUID + Age，拼成符号服务器键，如 `B2C7C3B72F30D3ED4C4C44205044422E1`（GUID 32 位十六进制 + Age 十六进制，无分隔符）。
2. 查本地缓存 `%LOCALAPPDATA%\ArborTab\offsets\<key>.json`。
3. 没有则启动后台线程，用 WinHTTP 下载 `https://raw.githubusercontent.com/<owner>/chrome-plugin-arbortab/offsets/win64/<key>.json`（备用：`https://cdn.jsdelivr.net/gh/<owner>/chrome-plugin-arbortab@offsets/win64/<key>.json`）。404 时每 30 分钟重试。
4. 取到后**在运行中直接启用功能**（无需重启 Chrome），并触发一次重新布局（§5.6）。
5. 每个函数在启用前比对 offsets 表中记录的前 16 字节（`prologue`）；不一致则不启用该函数相关功能并记日志。

### 5.3 运行时数据访问

全部偏移来自 offsets 表，代码里不写死。Chrome 156 的实测值见 §13.5，供开发时对照。

- **识别 TabView**：读 view 对象首 8 字节（虚表指针），与 offsets 表中 `TabView` 的全部虚表地址比较。
- **取 tabId**：
  `TabView::GetTabInterface(view)` → `TabInterface*` → 虚函数 `GetContents()`（虚表槽位号由 CI 计算）→ `sessions::SessionTabHelper::IdForTab(out, contents)`。
  `IdForTab` 是静态函数，**通过隐藏指针返回**：`rcx` = 输出 `SessionID*`（4 字节 int），`rdx` = `const WebContents*`。得到的值就是扩展里的 `tab.id`。
- **成员函数返回结构体的调用约定**（MSVC x64 / clang-cl）：`rcx`=this，`rdx`=返回值缓冲区，`r8` 起才是参数，返回 `rax`=返回值缓冲区。
- **`views::ProposedLayout`**（size 0x190）：
  - `+0x00` `host_size`（gfx::Size，两个 int）
  - `+0x08` `child_layouts`：**`absl::InlinedVector<ChildLayout, 8>`，不是 std::vector**（已踩坑）。
    - `+0x08` metadata = `(size << 1) | is_allocated`
    - `+0x10` 若 `is_allocated==0`：内联数组起点；否则：`{ ChildLayout* data; size_t capacity; }`
- **`views::ChildLayout`**（size 0x30）：`+0x00 child_view (View*)`、`+0x08 visible (bool)`、`+0x0c bounds (gfx::Rect: x,y,w,h int)`、`+0x1c available_size`。
- Chromium 使用 libc++（`std::__Cr`），**不要在接口上传递 MSVC STL 的 std::string/std::vector**。需要传字符串时只用 `std::u16string_view` 风格的 `{ptr, len}` 结构，或避免文本。

### 5.4 功能与拦截点

每个功能有独立开关（offsets 校验结果 + 用户配置）。

**F1 缩进与折叠隐藏** — `UnpinnedTabContainerViewLayout::CalculateProposedLayout`（虚函数，RVA 156: `0x0993c680`）

- 先调用原函数，再后处理返回的 ProposedLayout。
- 仅在 `this->orientation_ != 0`（垂直）时处理（反汇编确认：该字段非 0 走 `CalculateVerticalLayout`）。
- 遍历 child_layouts：只处理 TabView（跳过 TabGroupView、SplitTabView 等）。
  - 被隐藏的标签：`visible=false`，`bounds.height=0`。
  - 可见标签：`x += depth * indent`，`width -= depth * indent`（宽度保底，例如不小于 48px）。
  - 重新计算 y：按顺序累加可见项高度与原有间距（原间距从原始 layout 相邻项 y 差推得，不写死）；同步修正 `host_size.height`。
- 垂直栏收起（`TabView::collapsed_` 为 true）时 depth 视为 0。
- 原生实现里已有“不可见子项高度为 0、不占位”的分支（`IsChildVisibleInContainer` 返回 false 时），说明下游能正确处理 visible=false。
- 拖拽期间 DraggedTabsContainer 会读取布局做落点计算，**需要测试**隐藏项在拖拽时不导致错位。

**F2 点击图标折叠** — `TabView::OnMousePressed`（RVA 156: `0x09936f40`）

- 仅左键、无修饰键时检查：点击位置（`ui::LocatedEvent::location_`，相对 TabView 坐标）落在 `TabView::icon_` 子 view 的 bounds 内，且该标签在显示状态里 `hasChildren=true` → 发送 `twistyClick`，返回 `true`，**不调用原函数**（避免切换标签和开始拖拽）。
- 鼠标标志位 `ui::Event::flags_` 偏移来自 offsets 表。

**F3 折叠标记与数量** — `TabView::OnPaint` 或 `TabView::PaintChildren`（绘制后追加）

- 折叠的父标签上画一个明显的标记。最低要求：用 `gfx::Canvas::FillRect(const gfx::Rect&, SkColor)` 画一个小块/竖条。
- 可选：用 `gfx::Canvas::DrawStringRect(std::u16string_view, const FontList&, SkColor, const Rect&)` 画 `+N`。FontList 可取 TabTitle（views::Label）的字体；`u16string_view` 按 `{const char16_t* data; size_t size;}` 构造，注意 x64 下 16 字节结构按引用传递临时副本。
- 不修改标签标题文本（标题字符串是 libc++ 布局，且会被页面标题更新覆盖）。

**F4 拖入成为子标签** — `DraggedTabsContainer::HandleTabDragInContainer(this, const gfx::Rect& dragged_tab_bounds, const gfx::Point& point_in_container)`（RVA 156: `0x0990d5a0`，只有私有符号）

- `this` 是 `DraggedTabsContainer` 子对象（在 `UnpinnedTabContainerView` 内偏移 `0x340`），`host_view_` 在其 `+0x18`。
- 逻辑：取被拖矩形中心 y；在 host_view 当前子 view 中找一个**未被拖动**的可见 TabView，使中心 y 落在其 `[y + h/4, y + 3h/4]`：
  - 命中 → 记为 dropTarget，重绘目标，**不调用原函数**（压住原生重排）。
  - 未命中 → 清除 dropTarget（重绘旧目标），调用原函数。
- 第一次进入时向扩展发送 `dragBegin`。
- 被拖的父标签若展开：拖拽期间在 F1 中把其子孙临时视为隐藏。

**F5 拖拽结束** — `DraggedTabsContainer::OnTabDragEnded`（虚函数，有 public 符号）或 `TabDragHandlerImpl::EndDrag(EndDragReason)`

- 有 dropTarget 且不是取消 → 发送 `dropOnto {targetTabId}`；总是发送 `dragEnd`；清理状态。
- 如何区分“取消”（Esc）需要读源码 `tab_drag_handler.cc` / `tab_drag_controller.cc` 确认；优先选能拿到 `EndDragReason` 的拦截点。

**F6 落点高亮** — 与 F3 同一绘制拦截：`this == dropTarget` 时整块 FillRect 半透明强调色（颜色可配置，默认取系统强调色 40% 透明度）。

### 5.5 线程模型

- 所有 Views 对象只能在浏览器 UI 线程访问；拦截函数本身就在 UI 线程上运行。
- 第一次进入 F1 拦截时，在 UI 线程上创建一个**消息专用窗口**（`HWND_MESSAGE`），Chrome 的 UI 线程消息循环会派发它的消息。
- WebSocket 线程收到新状态 → 写入加锁的状态表 → `PostMessage` 给该窗口 → 窗口过程（UI 线程）对记录过的标签容器 view 调用 `views::View::InvalidateLayout` + `SchedulePaint`。
- 记录容器：F1 拦截时把 `host` view 指针加入集合；容器销毁后指针失效的问题，需在拦截里刷新集合（只保留本轮出现过的），或在调用前用 offsets 表里的虚表比对校验对象仍有效。

### 5.6 WebSocket 服务端

- 监听 `127.0.0.1:47821`（被占用时依次尝试到 `47825`），实现最小 RFC 6455：握手、文本帧、ping/pong、关闭；单帧消息即可（状态 JSON 通常 < 64KB，需支持扩展长度）。
- **来源校验**：只接受 `Origin: chrome-extension://<扩展ID>`，ID 白名单：商店版 ID + 本地开发版 ID（同一个 key 生成，通常相同），可在 `%LOCALAPPDATA%\ArborTab\config.ini` 追加。
- 允许多个客户端（多个 Chrome 配置文件各自运行一份扩展，但在同一个浏览器进程里）。UI 事件广播给所有客户端，扩展忽略不属于自己的 tabId。SessionID 在进程内唯一。
- JSON 解析用一个小型单头文件库（如 nlohmann/json 或 yyjson），注意只在 DLL 内部使用。

### 5.7 日志与诊断

- 日志：`%LOCALAPPDATA%\ArborTab\logs\native.log`（滚动，最多 1MB × 3）。
- 调试命令（仅开发版或配置开启）：扩展发送 `{"type":"debug.dumpLayout"}`，原生模块返回每个可见 TabView 的 `{tabId, bounds, visible}`，供自动化测试断言（§10）。
- 拦截函数内部做空指针与虚表校验，任何异常情况直接走原函数路径，不抛异常。

## 6. 通信协议（JSON over WebSocket）

扩展 → 原生：

```jsonc
{"type":"hello","protocol":1,"extVersion":"1.0.0"}
{"type":"state","windowTabs":[123,124,130],          // 本次快照涉及的全部 tabId
 "tabs":{"123":{"depth":0,"hidden":false,"hasChildren":true,"collapsed":true,"hiddenCount":3},
         "124":{"depth":1,"hidden":true}},
 "config":{"indent":16,"highlightColor":"#663D7EFF","showCount":true}}
{"type":"ping"}
```

原生 → 扩展：

```jsonc
{"type":"hello","protocol":1,"nativeVersion":"1.0.0","chrome":"156.0.8078.80",
 "features":{"layout":true,"twisty":true,"paint":true,"drag":true}}  // 未适配时全为 false
{"type":"twistyClick","tabId":123}
{"type":"dragBegin"}
{"type":"dropOnto","targetTabId":124}
{"type":"dragEnd","cancelled":false}
{"type":"pong"}
```

规则：

- 状态表中没有的 tabId 一律按 depth 0、可见处理。
- 原生模块重启后，扩展在 hello 后立即重发全部窗口状态。

## 7. offsets 表

### 7.1 格式

`offsets/win64/<PDB key>.json`：

```jsonc
{
  "schema": 1,
  "chromeVersion": "156.0.8078.80",
  "pdbKey": "B2C7C3B72F30D3ED4C4C44205044422E1",
  "generatedAt": "2026-11-08T00:00:00Z",
  "functions": {
    "UnpinnedTabContainerViewLayout::CalculateProposedLayout": {"rva": "0x0993c680", "prologue": "41574156565753 4883ec20 ..."},
    "TabView::OnMousePressed": {"rva": "...", "prologue": "..."},
    "...": {}
  },
  "vtables": { "TabView": ["0x0f998bd0", "..."] },
  "vtableSlots": { "TabInterface::GetContents": 0 },   // 示例值，实际由 CI 计算
  "fields": {
    "TabView.collection_node_": "0x418",
    "TabView.icon_": "0x438",
    "TabView.collapsed_": "0x61c",
    "UnpinnedTabContainerViewLayout.orientation_": "0x270",
    "views::ProposedLayout.child_layouts": "0x8",
    "views::ChildLayout.size": "0x30",
    "...": ""
  },
  "checks": { "ChildLayoutList": "absl::InlinedVector<views::ChildLayout,8>" }
}
```

### 7.2 解析清单（`resolver/spec.yaml`）

- **函数**：
  - 布局：`UnpinnedTabContainerViewLayout::CalculateProposedLayout`
  - 鼠标：`TabView::OnMousePressed`
  - 绘制：`TabView::OnPaint` 或 `TabView::PaintChildren`
  - 拖拽：`DraggedTabsContainer::HandleTabDragInContainer`、`DraggedTabsContainer::OnTabDragEnded` 或 `TabDragHandlerImpl::EndDrag`
  - 取 tabId：`TabView::GetTabInterface`、`sessions::SessionTabHelper::IdForTab`
  - 刷新：`views::View::InvalidateLayout`、`views::View::SchedulePaint`
  - 绘图：`gfx::Canvas::FillRect`（`Rect, SkColor` 重载）、（可选）`gfx::Canvas::DrawStringRect`、`views::Label::font_list`
  - 坐标：`views::View::ConvertPointFromScreen`（如需要）
- **虚表**：`TabView` 全部虚表、`tabs::TabModel` 主虚表（用于计算 `GetContents` 槽位：读取 chrome.dll 中该虚表的各项，找到等于 `tabs::TabModel::GetContents` 地址的那一项）。
- **字段**：§5.3、§5.4 用到的全部字段。
- **类型检查**：`views::ProposedLayout::child_layouts` 的类型名仍为 `absl::InlinedVector<views::ChildLayout,8,...>`，`ChildLayout` 大小为 0x30，等等。任何一项不符 → CI 失败。

### 7.3 解析注意事项

- 用 dbghelp（`SymLoadModuleExW` 直接加载 .pdb；`SymEnumSymbolsW`；`SymEnumTypesByNameW` + `SymGetTypeInfo`）。`research/tools/symq.py` 已验证可用，加载 5.7GB PDB 约 50 秒。
- **重载函数用修饰名（mangled name）精确匹配**（关闭 `SYMOPT_UNDNAME` 后枚举 public 符号）。例如：
  - `?ConvertPointFromScreen@View@views@@SAXPEBV12@PEAVPoint@gfx@@@Z`
  - `?FillRect@Canvas@gfx@@QEAAXAEBVRect@2@I@Z`
- 部分函数只有私有符号，没有 public 符号（如 `TabView::GetTabInterface`、`DraggedTabsContainer::HandleTabDragInContainer`）。此时用去修饰名匹配，并要求唯一。
- **检查同址折叠**：要拦截的函数地址不能与其他函数共用（用 `SymEnumSymbolsForAddr` 检查）；否则拦截会影响无关代码，该功能标记为不可用。
- `prologue` 从 chrome.dll 文件读取（RVA → 文件偏移用 pefile）。

## 8. CI（GitHub Actions，公开仓库）

### 8.1 `offsets.yml`（每小时 + 手动触发）

在 `windows-latest` 上运行（dbghelp 只在 Windows 上可用；公开仓库免费）：

1. **查询版本**：POST `https://tools.google.com/service/update2`，请求体：

   ```xml
   <?xml version="1.0" encoding="UTF-8"?>
   <request protocol="3.0" updater="Omaha" ismachine="1" installsource="ondemandcheckforupdate">
     <os platform="win" version="10.0" arch="x64"/>
     <app appid="{8A69D345-D564-463C-AFF1-A69D9E530F96}" version="" ap="x64-stable-multi-chrome" lang="en"><updatecheck/></app>
   </request>
   ```

   响应里有 `manifest version`、下载根地址 `url codebase` 和文件名 `package name`（2026-11-08 实测返回 157.0.8103.39）。可选：同样查询 Beta 渠道（`ap="x64-beta-multi-chrome"`），提前适配。
2. 若该版本已记录在 `offsets` 分支的 `index.json` 中，结束。
3. **下载并解包**：下载 `*_chrome_installer_uncompressed.exe`（约 530MB），用 7-Zip 解包：先得到 `chrome.7z`，再解出 `Chrome-bin/<version>/chrome.dll`。解包层级需实测确认。
4. **读取 PDB key**：用 `research/tools/pdbinfo.py` 的方法读取。
5. **下载 PDB**：`https://chromium-browser-symsrv.commondatastorage.googleapis.com/chrome.dll.pdb/<key>/chrome.dll.pdb`。服务端是 gzip 传输编码（约 1.08GB），**curl 需加 `--compressed`**，解压后约 5.7GB。runner 的 D: 盘空间足够。
6. 运行 `resolver/resolve.py` → 生成 JSON → 全部校验通过 → 提交到 `offsets` 分支：写入 `win64/<key>.json`，更新 `index.json`（版本 → key）。
7. 失败时：自动创建或更新 issue（标题含版本号），附上缺失或校验失败的项。这就是需要人工介入的信号。

### 8.2 `build.yml`（push / tag）

- 用 MSVC（`windows-latest` 自带）构建 `version.dll`，Release + `/MT` 静态 CRT。
- 打包扩展 zip。
- tag 推送时创建 GitHub Release，附带 `version.dll`、扩展 zip、`install.ps1`。
- 运行扩展的 node 单元测试。
- 可选：用当前 offsets 跑一次沙盒冒烟测试（§10）。

## 9. 安装与多设备

### 9.1 每台电脑首次安装

1. 正常安装 Chrome，登录同步。
2. 从 Chrome 应用商店安装 ArborTab。扩展本身和设置会随 Chrome 同步到其他设备。
3. 以管理员身份运行 `install.ps1`。它会：
   - 从最新 Release 下载 `version.dll`，放到 `C:\Program Files\Google\Chrome\Application\`（与 chrome.exe 同目录）；
   - 创建 `%LOCALAPPDATA%\ArborTab\`；
   - 注册一个登录时运行的计划任务：检查 `version.dll` 是否还在、是否是最新 Release，缺失时恢复。这是为了防止 Chrome 更新时清掉该文件（见 §12，待验证）。
4. 重启 Chrome。

`uninstall.ps1` 负责删除该文件和计划任务。

### 9.2 更新

- **Chrome 更新**：原生模块发现没有新版本的 offsets，就自动下载；CI 生成之前，功能暂时关闭，Chrome 按原样运行。
- **原生模块更新**：Chrome 运行时文件被占用，所以计划任务先把旧文件改名为 `version.dll.old`，再放入新文件，下次启动 Chrome 生效。
- **扩展更新**：走应用商店。

## 10. 开发与测试方法（已验证可行）

- **沙盒 Chrome**：把 `Application\chrome.exe` 和 `<version>\` 目录复制到一个独立目录（约 500MB），把开发中的 `version.dll` 放进去，启动参数：

  ```
  --user-data-dir=<临时目录> --no-first-run --no-default-browser-check
  --remote-debugging-port=9334 --enable-unsafe-extension-debugging
  ```

  - 在临时目录的 `Default\Preferences` 里预先写入 `{"vertical_tabs":{"enabled":true,"collapsed_state":false,"uncollapsed_width":240}}`，启动后直接显示垂直标签。
  - 这样完全不影响日常使用的 Chrome。
- **加载扩展**：通过 CDP 浏览器级 WebSocket 发送 `Extensions.loadUnpacked {path}`（`research/tools/cdp.py`）。Chrome 137+ 的正式版已不支持 `--load-extension`。
- **自动化断言**：
  - 用 CDP 的 `Target.createTarget` 等方法创建标签；
  - 通过扩展的调试接口或原生模块的 `debug.dumpLayout` 取得布局；
  - 断言缩进、隐藏、顺序是否正确。
- **截图核对**：用 PowerShell（`System.Drawing` 的 `CopyFromScreen`）按窗口矩形截图，人工核对。
- **崩溃分析**：
  - 沙盒配置文件下的 `Crashpad\reports\*.dmp` 可以用 `cdb -z <dmp> -c ".ecxr; r; kn"` 查看（Windows SDK 自带 cdb）；
  - 旧版 cdb 读不了新 PDB，地址可以用 `symq.py` 反查。
  - Windows 事件查看器的 Application 日志里也有崩溃记录。

## 11. 里程碑与验收

| # | 内容 | 验收 |
|---|---|---|
| M1 | resolver + spec，本地对 156 生成 offsets；CI `offsets.yml` 跑通并对 157 生成 | 两个版本的 JSON 都齐全，所有校验通过 |
| M2 | 原生模块：导出转发 + 浏览器进程识别 + offsets 加载（本地/下载）+ 功能开关 + 日志 | 沙盒 Chrome 正常运行，子进程无崩溃；删除 offsets 后 Chrome 原样运行 |
| M3 | F1 缩进/隐藏 + WebSocket + 线程模型 + 扩展树逻辑（创建/关闭子树/折叠快捷键） | 链接打开的子标签缩进；关闭父标签关闭子树；快捷键折叠后子标签隐藏、Ctrl+Tab 到隐藏标签自动展开 |
| M4 | F2 点击图标折叠 + F3 折叠标记 | 点图标切换折叠，不切换激活标签；折叠父标签有明显标记与数量 |
| M5 | F4/F5/F6 拖拽 | 中部松手成为子标签并高亮；上下边缘为原生排序且父节点推断正确；Esc 取消无副作用；拖展开的父标签时子树跟随 |
| M6 | 安装脚本、计划任务、Release、应用商店上架材料、README | 在第二台电脑按 README 完成安装并通过 M3–M5 的手动检查 |

## 12. 风险与待验证项

1. **Chrome 自动更新后 `version.dll` 是否保留**：未验证。用户的 Chrome 即将从 156 更新到 157，正好可以观察。计划任务兜底。
2. **杀毒软件**：`version.dll` 代理加载方式可能被误报，README 说明添加排除项。
3. **拖拽期间隐藏项对落点计算的影响**（F1×F4）：需实测。
4. **本地网络访问策略**：目前扩展声明 `host_permissions` 后不提示，将来 Chrome 策略可能变化。备选方案：Native Messaging（扩展 ↔ 独立宿主进程 ↔ 命名管道 ↔ 原生模块）。协议层保持不变，便于切换。
5. **Chrome 重构垂直标签实现**：函数改名或删除会导致 CI 失败，需要人工按新源码调整 spec 和功能代码。源码可以用浅克隆 + sparse checkout 拉取指定 tag（见 §13.1）。
6. **多配置文件、多窗口、隐身窗口**：隐身窗口里扩展默认不运行，原生模块对没有状态的标签按原样显示，所以不受影响。

## 13. 前期调研结论与资料

### 13.1 源码

- 本地有 Chromium 156.0.8078.80 的部分源码：`C:\Users\evative7\Desktop\Playground\_ref\chromium`。是浅克隆 + sparse checkout，只取了以下目录：
  - `chrome/browser/ui/views/tabs/`
  - `chrome/browser/ui/tabs/`
  - `chrome/browser/ui/views/frame/`
  - `components/tabs/`
  - `ui/views/layout/`
  - `components/sessions/content/`
- 获取方法：`git fetch --depth 1 --filter=blob:none origin refs/tags/<version>`，远端是 `https://github.com/chromium/chromium.git`，然后 `git sparse-checkout set --no-cone ...`。
- 关键文件：
  - `chrome/browser/ui/views/tabs/common/unpinned_tab_container_view_layout.cc`：垂直布局，含“不可见子项高度为 0”的分支
  - `.../common/tab_view.{h,cc}`：`OnMousePressed`、`GetTabInterface`、`icon_`、`collapsed_`
  - `.../common/tab_view_vertical_layout.cc`：标签内部布局
  - `.../common/dragged_tabs_container.cc`：`HandleTabDragInContainer`、`GetTargetForTabDrag`；原生拖拽是“跨过中线即重排”的实时重排
  - `.../common/tab_drag_handler.{h,cc}`：`InitializeDrag`、`HandleDraggedTabsIntoPosition`、`EndDrag`
  - `ui/views/layout/proposed_layout.h`：`ChildLayoutList` = `absl::InlinedVector`
  - `components/sessions/content/session_tab_helper.h`：`IdForTab`
- 垂直标签的偏好设置键：`vertical_tabs.enabled`，在 Preferences 里。

### 13.2 调研工具（已复制到 `research/`）

- `research/tools/pdbinfo.py`：读 chrome.dll 的 PDB key。用法：`uv run --with pefile python -I pdbinfo.py <chrome.dll>`
- `research/tools/symq.py`：dbghelp 查询。
  - `sym <通配名>`：查符号；设置 `RAW=1` 环境变量时按修饰名查
  - `type <类型名>`：查结构体字段偏移
  - 用法：`uv run python -I symq.py <pdb> sym|type ...`
- `research/tools/dis.py`：反汇编指定 RVA。用法：`uv run --with pefile --with capstone python -I dis.py <dll> 0xRVA:len`
- `research/tools/server.py` + `research/lna-ext/` + `research/tools/cdp.py`：WebSocket 连通性实验
- `research/poc/`：概念验证用的 `version.dll`。包含导出转发、浏览器进程识别、等待 chrome.dll、拦截布局函数并让所有标签右移 20px。
  - `build.bat` 里 MinHook 的路径指向 `..\..\_ref\minhook`，需要改成仓库内的路径。
  - **最后一版**（已修正 InlinedVector 的处理）**尚未运行验证**。

本地 PDB 缓存：`C:\Users\evative7\Desktop\Playground\_dl\pdb\chrome.dll.pdb`（156，5.7GB）。

### 13.3 已实测的事实

- `chrome.exe` 的导入表有 `VERSION.dll`，用到的函数是 `GetFileVersionInfoSizeW`、`GetFileVersionInfoW`、`VerQueryValueW`。同目录放置的代理 DLL 会被加载进浏览器进程。
- 在 chrome.dll 加载通知里安装 MinHook 拦截可行，拦截函数会被正常调用。
- 扩展 service worker 连接 `ws://127.0.0.1` 时：manifest 声明了 `host_permissions`，所以**没有权限提示**；服务端收到的是 `Origin: chrome-extension://<id>`。
- Chrome 符号服务器上有正式版 chrome.dll 的完整私有 PDB，包含类型信息。
- 本机工具链：
  - VS 2022 BuildTools（MSVC 14.44，`vcvars64.bat`），Windows SDK 10.0.26100
  - cmake、uv
  - cdb，位于 `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`

### 13.4 已踩过的坑

1. 系统 `version.dll` 必须在 `DllMain` 中加载。延迟加载会导致沙箱子进程调用空指针崩溃。
2. `child_layouts` 是 `absl::InlinedVector`，按 `std::vector` 读会崩溃。
3. 在 bash heredoc 里写 C 字符串时，`L"\\version.dll"` 被错误转义成了 `L"\version.dll"`，生成文件后要检查。

### 13.5 Chrome 156.0.8078.80 实测数据（PDB key `B2C7C3B72F30D3ED4C4C44205044422E1`）

函数 RVA：

```
UnpinnedTabContainerViewLayout::CalculateProposedLayout  0x0993c680  prologue 41 57 41 56 56 57 53 48 83 ec 20
UnpinnedTabContainerViewLayout::CalculateVerticalLayout  0x0993cf00
TabGroupViewLayout::CalculateProposedLayout              0x0992cc50
TabViewVerticalLayout::CalculateProposedLayout           0x0993a6c0
TabView::OnMousePressed                                  0x09936f40
TabView::GetTabInterface                                 0x09935d70   (读取 this+0x418)
TabView::OnPaint                                         0x09937760
TabView::PaintChildren                                   0x09937680
TabView::UpdateHovered                                   0x09936740
DraggedTabsContainer::HandleTabDragInContainer           0x0990d5a0
DraggedTabsContainer::GetTargetForTabDrag                0x0990f4f0
DraggedTabsContainer::OnTabDragEnded                     0x0990d720
TabDragHandlerImpl::InitializeDrag / ContinueDrag / EndDrag  0x09922870 / 0x09923780 / 0x099238b0
TabDragHandlerImpl::HandleDraggedTabsIntoPosition        0x099257b0
sessions::SessionTabHelper::IdForTab                     0x019d7ba0   (rcx=out SessionID*, rdx=WebContents*)
tabs::TabModel::GetContents                              0x04b48990   (mov rax,[rcx+0x40]; ret)
views::View::InvalidateLayout                            0x044a1090
views::View::SchedulePaint                               0x047a09d0
gfx::Canvas::FillRect(const Rect&, SkColor)              0x019ce590
TabView 虚表（之一）                                      0x0f998bd0 等 13 个
```

字段偏移：

```
TabView (size 0x690): collection_node_ +0x418, title_ +0x430, icon_ +0x438, close_button_ +0x448,
                      active_ +0x618, selected_ +0x619, hovered_ +0x61a, collapsed_ +0x61c, pinned_ +0x61d
UnpinnedTabContainerViewLayout: orientation_ +0x270
UnpinnedTabContainerView (size 0x588): DraggedTabsContainer 子对象 +0x340
DraggedTabsContainer: host_view_ +0x18, collection_node_ +0x20, dragging_views_ +0x48
TabCollectionNode: type_ +0xa8, node_data_ +0xb8, children_ +0xc8, node_view_ +0x100
ui::Event: flags_ +0x50;  ui::LocatedEvent: location_ +0xb0 (gfx::PointF? 需按类型确认)
views::ProposedLayout (0x190): host_size +0x0, child_layouts +0x8 (InlinedVector<ChildLayout,8>)
views::ChildLayout (0x30): child_view +0x0, visible +0x8, bounds +0xc, available_size +0x1c
```

ArborTab — 在 Chrome 原生垂直标签栏中使用树状标签

仅支持 Windows x64，需要 Chrome 原生垂直标签栏。完整的视觉集成需要另外从 GitHub 安装开源原生模块；安装前请阅读下方安全说明。

ArborTab 在 Chrome 自己的垂直标签栏中维护真正的父子树，无需侧边栏，也不会再显示一份重复的标签列表。

功能：

- 从链接打开的标签自动成为来源标签的子标签；
- 折叠和展开整棵子树；
- 关闭父标签时一并关闭所有后代；
- 把标签拖到另一个标签上成为其子项，或拖到边缘调整兄弟顺序；
- 缩进引导线、隐藏标签计数、主题配色和可选动画；
- 折叠、关闭子树、增加和减少层级的快捷键；
- 支持八种界面语言。

只安装扩展也可以维护树、调整真实标签顺序、使用快捷键和关闭子树；但在安装原生模块前，Chrome 标签栏在视觉上仍是平的。

原生模块安装：

安装扩展后打开 ArborTab 设置页，其中会提供最新 GitHub Release 链接和安装命令。源代码、安装脚本、软件包、校验和及卸载方式均公开：
https://github.com/EvATive7/chrome-plugin-arbortab

安装命令：
powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"

安全说明：

原生组件是安装在 `chrome.exe` 同目录的 `version.dll` 代理，会接入 Chrome 垂直标签栏的布局、绘制、鼠标和拖拽处理。由于当前分发的 DLL 没有 Microsoft 签名，它会为 Chrome 启动的子进程关闭“只允许 Microsoft 签名二进制文件”的一项缓解措施，因而降低这些进程的代码完整性保护。请只在审阅并接受这一取舍后安装。原生包提供卸载程序。

隐私：

ArborTab 没有广告、分析或项目方运营的服务器，不上传浏览数据。扩展通过 Chrome Native Messaging 与本机模块通信。原生模块会从 GitHub/jsDelivr 下载经过签名的兼容数据，但不会上传标签网址、标题或树。诊断报告只保留在本机，除非用户主动导出并分享。

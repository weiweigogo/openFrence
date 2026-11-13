# openFences

> 一个轻量级的 Windows 桌面图标分组管理工具，Stardock Fences 的开源替代。
>
> A lightweight Windows desktop icon grouping tool — an open-source alternative to Stardock Fences.

![platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![lang](https://img.shields.io/badge/language-C%2B%2B17-%23f34b7d)
![size](https://img.shields.io/badge/size-~780KB-green)

---

## 特性 / Features

- **桌面图标分组** — 在桌面上创建半透明毛玻璃围栏（Fence），将图标拖入其中自动归组
- **毛玻璃效果** — Acrylic 风格背景：降采样模糊 + 饱和度增强 + 噪点，贴近 Win11 原生观感
- **围栏放大** — 单击围栏空白处进入类手机文件夹的大图标视图：围栏飞到工作区中央、图标放大并居中重排，桌面其余部分模糊；单击围栏内空白或围栏外任意位置即可关闭
- **Win11 风格右键菜单** — 自绘 Fluent Design 菜单，带圆角、毛玻璃背景、软阴影
- **系统托盘常驻** — 托盘图标右键菜单：新建围栏、隐藏/显示全部、切换语言、开机自启、桌面快捷方式
- **配置持久化** — 围栏布局、图标位置、外观设置自动保存至 `%APPDATA%\openFences\config.json`
- **多语言** — 支持中文 / English 切换
- **极轻量** — 单 exe（~780KB），无依赖，无需安装 VC++ 运行库，拷贝即用

---

## 快速开始 / Quick Start

1. 下载 [openfences.exe](https://github.com/weiweigogo/openFrence/releases)（最新 Release）
2. 放到任意目录，双击运行
3. 右键右下角托盘图标 → 设置语言、开机自启、创建桌面快捷方式
4. 在桌面上拖拽图标到围栏中即可

> **提示**：如果桌面图标被隐藏导致围栏消失，右键托盘 →「显示桌面图标」即可恢复。围栏会自动停靠在 DefView 上等待图标列表恢复。

### 围栏放大 / Fence Zoom

单击围栏内部的空白区域，围栏会飞到显示器工作区的 58%×66%，图标最大放大 2.4 倍并按行居中，桌面其余部分显示模糊压暗的遮罩。单击围栏内部空白或围栏外部任意位置即可恢复。

放大是瞬态交互，不会改变围栏保存的位置和大小；放大期间图标拖动和边缘缩放会暂停，避免把临时布局写回配置。

---

## 构建 / Build

```bash
# 前置条件：Visual Studio 2017+（含 Windows 10 SDK）、CMake 3.20+

git clone https://github.com/zw/openFences.git
cd openFences/cpp
cmake -B build -G "Visual Studio 15 2017" -A x64
cmake --build build --config Release --target openfences
```

产物：`cpp/build/Release/openfences.exe`（~780KB，静态链接 CRT）

---

## 项目结构 / Project Layout

```
openFences/
├── cpp/
│   ├── CMakeLists.txt          # CMake 构建配置
│   ├── app.ico                 # 应用程序图标（可替换）
│   └── src/
│       ├── main.cpp            # 入口、托盘、桌面管理
│       ├── fence_window.cpp    # Fence 窗口（创建/拖拽/缩放）
│       ├── render.cpp          # Direct2D 渲染（毛玻璃/Acrylic）
│       ├── context_menu.cpp    # Win11 Fluent 自绘右键菜单
│       ├── settings_panel.cpp  # 外观设置面板
│       ├── config.cpp          # JSON 配置读写、开机自启
│       ├── icon_extract.cpp    # 图标提取
│       ├── menu_icons.cpp      # 菜单矢量图标光栅化
│       └── resource.h          # 资源 ID 定义
├── CLAUDE.md                   # 开发指引（供 AI 辅助）
└── README.md
```

---

## 配置 / Configuration

配置文件位置：`%APPDATA%\openFences\config.json`

```json
{
  "version": 1,
  "language": 1,
  "hideAll": false,
  "fences": [
    {
      "id": "f1",
      "title": "应用",
      "x": 75, "y": 75,
      "w": 420, "h": 420,
      "collapsed": false,
      "icons": [ ... ],
      "bg": [0.08, 0.08, 0.10, 0.55],
      "titleColor": [0.05, 0.05, 0.07, 0.80],
      "fontSize": 18.0,
      "titleAlign": 0
    }
  ]
}
```

---

## 技术栈 / Tech Stack

- **语言**：C++17
- **窗口**：Win32 `WS_CHILD` + `WS_EX_LAYERED` + `UpdateLayeredWindow`
- **渲染**：Direct2D、DirectWrite、WIC
- **JSON**：[nlohmann/json](https://github.com/nlohmann/json)
- **构建**：CMake + MSVC（VS2017+），x64，静态链接 CRT（`/MT`）

---

## 许可证 / License

[MIT](LICENSE)

---

## 致谢 / Acknowledgments

- 图标渲染管线参考了 [nlohmann/json](https://github.com/nlohmann/json) 的设计理念
- Fluent Design 视觉规范受到 Microsoft WinUI 设计语言的启发
- Stardock Fences 的原设计为此项目提供了灵感

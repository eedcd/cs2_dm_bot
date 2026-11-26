# CS2 DM Bot

[![Build](https://github.com/eedcd/cs2_dm_bot/actions/workflows/build.yaml/badge.svg?branch=main)](https://github.com/eedcd/cs2_dm_bot/actions/workflows/build.yaml)
[![Offsets](https://github.com/eedcd/cs2_dm_bot/actions/workflows/update-files.yml/badge.svg?branch=main)](https://github.com/eedcd/cs2_dm_bot/actions/workflows/update-files.yml)
[![License](https://img.shields.io/github/license/eedcd/cs2_dm_bot)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)

一个基于 SDL2、Dear ImGui 和 C++20 的 CS2 外部 DM Bot：无交互菜单，只有右上角状态徽标；开启后自动 ESP、自瞄、扳机和地图寻路移动。

**中文** | [English](README_EN.md)

> [!WARNING]
> 本项目仅用于逆向工程与图形渲染学习。仅可在使用 `-insecure` 启动的离线环境中运行，禁止用于 VAC 服务器或任何在线游戏。使用者须自行承担不当使用造成的风险。

## 功能

| 模块 | 功能 |
| --- | --- |
| 状态徽标 | 右上角常驻置顶；无 CS2 进程时黄框黑字 `DM Bot waiting for CS2 process`，已连接时蓝框 `DM Bot disable` / `DM Bot enable` |
| ESP | 方框、骨骼、血量、距离、武器、视角、可见性判断；**不分阵营**，显示除自己以外的所有 CT / T |
| 自瞄 | 开启后常驻；360° 搜索、优先角度最近、只锁真实可见目标（分层地图视线检测，不隔墙锁） |
| 扳机 | 开启后常驻；锁定目标后随机 100–300 ms 开火 |
| 移动 | 用地图雷达掩码做 A\* 寻路，优先前往最近的存活玩家；无目标时循环巡逻；撞墙自动绕行；开火前反向键急停 |
| 地图数据 | 内置 16 张常用地图 + 4 张分层地图（Nuke / Train / Vertigo / Baggage 下层） |

## 系统要求

- Windows 10 或 Windows 11（x64）
- Visual Studio 2022（或更新版本），安装“使用 C++ 的桌面开发”工作负载
- CS2 使用 `-insecure` 参数与全屏窗口化模式

覆盖层仅支持完整位于单个显示器中的游戏窗口。跨显示器时会暂停渲染，窗口恢复后自动继续；独占全屏不受支持。程序启动时会请求管理员权限，并能在 CS2 重启后自动重新连接。

## 快速开始

1. 从 [Releases](https://github.com/eedcd/cs2_dm_bot/releases/latest) 下载并解压最新版本。
2. 确认 `external-cheat-base.exe` 同目录下存在 `maps\` 文件夹（随压缩包提供）。
3. 使用 `-insecure` 参数启动 CS2 并进入离线对局。
4. 运行 `external-cheat-base.exe`，接受 Windows 管理员权限提示。

| 按键 | 操作 |
| --- | --- |
| `F4` | 在 `DM Bot disable` 与 `DM Bot enable` 之间循环切换 |
| `F9` | 退出程序 |

开启（`DM Bot enable`）后立即启用 ESP、自瞄、扳机与自动寻路移动；关闭后所有功能停止并释放移动按键。

如明确需要写内存的防闪光功能，可显式允许内存写入（默认关闭）：

```powershell
.\external-cheat-base.exe --allow-memory-writes
```

运行日志位于 `%TEMP%\cs2-esp.log`。

## 寻路与视线检测

- 地图雷达图（`assets/maps/*.png`）只绘制可行走地面，程序把它直接当作寻路栅格：
  - 512×512 网格、腐蚀 1 格保留墙距、八方向 A\*、视线平滑压缩路径。
  - 通过 `dwGlobalVars` 读取当前地图名，选择对应的 `originX / originY / scale` 做世界坐标 ↔ 像素坐标转换。
  - 每 600 ms 或目标移动超过 250 单位时重算路径；距离目标 150 以内停止移动。
- 分层地图按 Z 阈值选择上层或下层掩码；两人不同楼层时两层都检查（允许楼梯 / 斜坡通视）。
- 隔墙检测对头部、胸部、脚部三点分别做射线检测，任意一点通视即判定可见。
- 外部进程无法调用游戏引擎 `TraceShape`，因此视线基于雷达可行走掩码近似；也不解析 VPK 三维碰撞几何。

不在内置列表中的地图会退化为“追最近玩家 + 撞墙绕行”。

## 构建

### Visual Studio

```powershell
git clone https://github.com/eedcd/cs2_dm_bot.git
```

使用 Visual Studio 打开 `external-cheat-base.sln`，选择 `Release | x64` 构建。构建后会自动把 `SDL2.dll` 与 `assets/maps` 复制到输出目录。

命令行构建：

```powershell
msbuild external-cheat-base.sln /p:Configuration=Release /p:Platform=x64
```

产物位于 `x64\Release\`，必须保留同目录的 `maps\` 文件夹。

## 项目结构

| 路径 | 内容 |
| --- | --- |
| `external-cheat-base/` | Windows C++ 主程序、覆盖层、ESP、自瞄、扳机与移动 |
| `assets/maps/` | 地图雷达掩码（16 张地图 + 4 张分层图），构建时复制到输出目录 |
| `tests/` | C++ 回归测试（视口数学、采样时序） |
| `docs/` | 原理与逆向学习文档 |
| `.github/workflows/` | 构建、发布与偏移自动更新 |

关键模块：

- `src/features/esp.cpp`：实体读取与 ESP 渲染
- `src/features/aimbot.cpp`：自瞄、扳机与可见性检测
- `src/features/movement.cpp`：自动移动、急停与卡墙绕行
- `src/features/navigation.cpp`：雷达掩码解码、A\* 寻路、分层视线
- `src/main.cpp`：状态徽标、F4/F9 与数据/渲染循环

## CI 与自动更新

- **Build** 徽章由 GitHub Actions 生成：绿色表示 `main` 最近一次构建成功。
- CI 在 Ubuntu 上运行 C++ 回归测试，在 Windows 上执行 `Release | x64` 编译、校验发布目录并打包 Release。
- **Offsets** 工作流每小时检查一次 [cs2-dumper](https://github.com/a2x/cs2-dumper)。按确切提交 SHA 获取并校验头文件，提交信息格式为 `build: update CS2 offsets to <sha>`，随后触发完整构建。

## 许可证与第三方资产

项目代码以 [MIT License](LICENSE) 发布。

地图图片与 overview 坐标归 Valve 所有，并继续受 Valve 的版权和条款约束。主要依赖与数据来源：[cs2-dumper](https://github.com/a2x/cs2-dumper)、[SDL](https://github.com/libsdl-org/SDL)、[Dear ImGui](https://github.com/ocornut/imgui) 和 [awpy-data](https://github.com/pnxenopoulos/awpy-data)。

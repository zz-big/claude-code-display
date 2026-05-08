# Claude Code Display

> 一块 WiFi 连网的小 OLED 屏，告诉你 Claude Code 啥时候需要你看一眼 —— 不用再死盯终端。

[English →](README.md)

<p align="center">
  <img src="docs/images/oled-running.jpg" alt="OLED 实物，WORKING 状态" width="420">
</p>

<sub>实拍 —— `WORKING` 状态，正在编辑 `README.zh.md`，右上角显示已用时长。还有[短视频演示](docs/videos/demo.mp4)。</sub>

## 前置条件

- **Claude Code ≥ 2.0** —— 老版本没有 `PermissionRequest` hook 事件，权限弹窗时屏幕不会闪 WAITING。`claude --version` 查版本。
- **macOS / Linux / Windows** 都可以。[`hooks/`](hooks/) 下提供两个等价实现：
  - **`claude-display.sh`** —— POSIX bash 脚本。macOS/Linux 原生用，Windows 在 **Git Bash** 或 **WSL** 里也能跑。
  - **`claude-display.ps1`** —— PowerShell 5.1+ 端口。**原生 Windows** 用（PowerShell 或 `cmd.exe` 启动都行，不需要装 Git Bash）。两个脚本共用同一个 conf 文件、同一套环境变量、同一种 payload —— 看你 shell 习惯任挑一个。
- **ESP32-C3 Super Mini**（或任何带 2.4 GHz WiFi 的 ESP32）+ **0.96" SSD1306 OLED**。
- **2.4 GHz WiFi**（ESP32-C3 没有 5 GHz）。
- 主机端：bash 版需要 `curl`（Git Bash 自带），建议装 `jq`；PS 版需要 PowerShell 5.1+（Windows 自带）。固件烧录要 `arduino-cli`（`brew install arduino-cli` / `winget install ArduinoSA.CLI`）或 Arduino IDE。

## Claude Code 用户 —— 一行命令装好

```bash
git clone https://github.com/zz-big/claude-code-display.git
cd claude-code-display
claude
```

然后在 Claude Code 里直接说：

> 帮我装一下

仓库里带 [`CLAUDE.md`](CLAUDE.md) 和 [`SKILL.md`](SKILL.md) —— Claude Code 自动读，整套流程都帮你做：

- **烧固件（按需，要你点头）** —— Claude 用 `arduino-cli` 装库、问你 WiFi 用户名密码和时区、写 `config.h`、找串口，编译 + 上传。开始前 ESP32 用 USB 接上电脑就行。
- **Mac 端配置（必做）** —— 拷 hook 脚本到 `~/.claude/hooks/`、写设备 URL 配置（mDNS 不通时自动改成 IP）、合并 hooks 到 `~/.claude/settings.json`、跑测试。

如果你已经手动烧好了，或者想自己用 Arduino IDE 烧，直接告诉 Claude，它会跳过烧录这步只做 Mac 端配置。手动烧录步骤在下面 [§ 快速上手 → 烧录固件](#1--烧录固件)。

## 它能干嘛

Claude Code 工作时，硬件屏幕一眼就能看出当前在啥状态：

| 状态        | 视觉效果                              | 触发时机                                       |
|-------------|---------------------------------------|-----------------------------------------------|
| **WAITING** | 整屏反白闪烁，每 400ms 切换           | Claude 在请求工具使用许可                      |
| **WORKING** | 旋转点图标 + 工具名                   | 正在跑工具（`Bash`、`Edit foo.py` 等）         |
| **DONE**    | 对勾 + "took 14s"                     | 一轮回复结束                                   |
| **ERROR**   | X 图标 + 250ms 急速反白闪烁           | 工具调用返回错误                               |
| **IDLE**    | 时钟 + 今日统计                       | 没事干                                         |

多个 Claude Code 同时跑会被分别追踪，屏幕上轮播显示。**WAITING 永远优先** —— 只要有任何 session 需要你处理，屏幕就锁定在它上面闪烁，直到你处理完。

UTF-8 中文（项目名、错误消息等）通过 u8g2 + GB2312 字体正常渲染。

## 硬件清单

- **ESP32-C3 Super Mini**（或任意支持 2.4 GHz WiFi 的 ESP32 板）
- **0.96" OLED 屏，SSD1306 驱动，128×64，I2C**
- USB-C 数据线

| OLED 模块 | ESP32-C3 Super Mini 引脚图 |
|---|---|
| <img src="docs/images/oled-module.jpg" width="280"> | <img src="docs/images/esp32-c3-pinout.jpg" width="280"> |

接线（默认引脚，要改去 `firmware/claude_status/claude_status.ino` 里调）：

| OLED  | ESP32-C3 Super Mini |
|-------|---------------------|
| GND   | GND                 |
| VCC   | 3V3                 |
| SCL   | GPIO 9              |
| SDA   | GPIO 8              |

## 快速上手

### 1 · 烧录固件

Arduino IDE → 库管理器装：
- **U8g2** by Oliver
- **ArduinoJson** by Benoit Blanchon

板子包：开发板管理器搜 "esp32" 装 by Espressif。

配置 WiFi：
```bash
cd firmware/claude_status
cp config.h.example config.h
# 编辑 config.h，填你的 WiFi 和时区
```

打开 `firmware/claude_status/claude_status.ino`：
- 板型：**ESP32C3 Dev Module**
- 分区方案：默认即可（`Default 4MB with spiffs`）
- 上传

烧好后 OLED 会显示设备的 IP 地址。同时 mDNS 名 `http://claude-display.local` 也能访问。

### 2 · 安装 hook 脚本

**macOS / Linux / Git Bash / WSL**：
```bash
mkdir -p ~/.claude/hooks
cp hooks/claude-display.sh ~/.claude/hooks/
chmod +x ~/.claude/hooks/claude-display.sh
```

**原生 Windows（PowerShell）**：
```powershell
$dir = Join-Path $HOME '.claude/hooks'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
Copy-Item hooks/claude-display.ps1 $dir
```

然后在 `~/.claude/settings.json` 里把每个 hook 的 `command` 字段指向对应脚本。bash 版用 `~/.claude/hooks/claude-display.sh waiting`；PS 版用 `pwsh -NoProfile -ExecutionPolicy Bypass -File "$env:USERPROFILE\.claude\hooks\claude-display.ps1" waiting`。完整 hooks 块见 [`examples/settings.json`](examples/settings.json)（bash 形式）—— 原生 Windows 上把 `command` 字符串换成上面的 pwsh 形式即可。

如果 `claude-display.local` 在你网络里解析不到（公司 WiFi、客网这类常会屏蔽 mDNS），把设备 URL 写到 hook 配置文件里：
```bash
cat > ~/.claude/hooks/claude-display.conf <<'EOF'
CLAUDE_DISPLAY_URL=http://192.168.x.x/status
EOF
```
IP 看 OLED 启动时底部那行。Hook 脚本会运行时 source 这个文件，比 `~/.zshrc` 更靠谱 —— 因为 Claude Code 派生 hook 子进程时不一定会继承 shell rc 里的环境变量。

### 3 · 配 Claude Code hooks

把 [`examples/settings.json`](examples/settings.json) 的 `hooks` 块合并到 `~/.claude/settings.json`。然后**完全退出并重启 Claude Code** —— hooks 在会话启动时加载一次。

### 4 · 测试

Claude Code 里随便发条消息，屏幕应该切到 WORKING。

或者绕开 Claude Code 直接测：
```bash
curl -X POST http://claude-display.local/status \
  -H "Content-Type: application/json" \
  -d '{"state":"waiting","msg":"需要许可","session":"demo"}'
```

屏幕应该立刻进入 WAITING 闪烁状态。然后清理：
```bash
curl -X POST http://claude-display.local/clear
```

> **Claude Code 用户**：项目里有 [`SKILL.md`](SKILL.md)，让 Claude 自己帮你装好 Mac/Linux 这边的配置。

## 工作原理

```
Claude Code 事件        →   shell hook              →   curl POST   →   ESP32 HTTP server  →   OLED
   （PreToolUse 等）        （~/.claude/hooks/...）       （走局域网）       （按 session 维护状态）   （远远就能看到）
```

事件 → 状态映射（见 [`examples/settings.json`](examples/settings.json)）：

| Claude Code 事件     | OLED 状态 |
|---------------------|-----------|
| `SessionStart`      | IDLE      |
| `UserPromptSubmit`  | WORKING   |
| `PreToolUse`        | WORKING   |
| `PermissionRequest` | **WAITING**（反白闪烁——需要你处理） |
| `PostToolUse`       | WORKING（工具失败时是 ERROR） |
| `Stop`              | DONE      |
| `SessionEnd`        | IDLE      |

> **关于 Claude Code 版本** —— `PermissionRequest` 是 **2.0+** 引入的 hook 事件。更早版本的权限弹窗走 `Notification`，但当前 `Notification` 同时还覆盖 idle 60s、auth、elicitation 等无关通知，全映射成 WAITING 会乱闪，所以我们故意不订阅 `Notification`。如果你 Claude Code 低于 2.0，OLED 不闪 WAITING 就是这个原因。

Hook 脚本的 curl 是后台执行的，1 秒超时 —— 设备掉线也不会卡住 Claude Code。固件 5 分钟没收到事件会把 WORKING 自动回退到 IDLE（处理长 build 中途没事件、用户中断、丢失 Stop hook 等情况）。

## HTTP API

| 端点          | 方法 | 描述                                                          |
|---------------|------|---------------------------------------------------------------|
| `/status`     | POST | 推送状态，JSON: `{state, msg, project, session}`              |
| `/status`     | GET  | 查所有活跃 session + 今日统计                                  |
| `/clear`      | POST | 清掉所有活跃 session                                          |
| `/`           | GET  | 简单 HTML 状态页                                               |

状态取值：`idle` · `working` · `waiting` · `done` · `error`

## 配置项

### 固件（`firmware/claude_status/config.h`）
- `WIFI_SSID`、`WIFI_PASSWORD`
- `MDNS_NAME` —— mDNS 主机名（默认 `claude-display`）
- `TZ_OFFSET_SEC` —— UTC 时区偏移（秒），例如中国是 `8 * 3600`

### Hook 脚本配置
持久化配置写在 `~/.claude/hooks/claude-display.conf`（按 POSIX shell 格式 source）。模板见 [`examples/claude-display.conf.example`](examples/claude-display.conf.example)。
- `CLAUDE_DISPLAY_URL` —— 设备 `/status` 完整 URL。默认 `http://claude-display.local/status`。
- `CLAUDE_DISPLAY_LOG` —— 日志路径。默认 `~/.claude/hooks/claude-display.log`。
- `CLAUDE_DISPLAY_LOG_MAX_LINES` —— 日志轮转阈值。默认 2000；超过 2N 行就只留最后 N 行。

三者都可以当环境变量设（环境变量优先级高于配置文件）。

### 安全模型 —— 信任本地网络
HTTP API **不带任何鉴权**。同一 LAN 上的人都能 POST `/status`（改屏内容）或 `/clear`（清 session）。这是为家庭网络小玩具设计的折衷 —— 加 token 会让安装变复杂。**别把设备暴露到公网**，咖啡厅 / 会议这种不可信网络下注意被恶搞屏的风险。

### 固件常量（在 `.ino` 里，一般不动）
- `MAX_SESSIONS` —— 同时追踪的 session 数（默认 4）
- `SESSION_ROTATE_MS` —— 多 session 轮播间隔
- `WORKING_TIMEOUT_MS` —— WORKING 多久没更新自动回 IDLE
- `DONE_LINGER_MS` —— DONE 显示多久后让位时钟

## 常见问题

**屏幕一直停在 WAITING 不动**
有孤儿 session 没被覆盖（多半是手动 curl 测试留下的）。`curl -X POST http://claude-display.local/clear` 清掉。真实 Claude Code 事件不会留孤儿，因为后续 hook（`PreToolUse`、`Stop` 等）会覆盖该 session 的状态。

**WiFi 连不上**
- ESP32-C3 **只支持 2.4 GHz**。如果你的 SSID 只在 5 GHz，连不上。
- 换个 USB 电源试试。ESP32-C3 Super Mini 在供电边缘时 RF 会被复位。

**OLED 一片空白或乱码**
- 默认字体（`u8g2_font_wqy12_t_gb2312`）占 ~80 KB flash。如果你板子的 app 分区小，可以改成 `u8g2_font_wqy12_t_chinese3`（~50 KB，覆盖少一些不常用的字）。

**编译报 `text section exceeds available space in board`**
Arduino IDE → Tools → Partition Scheme 改成 **Huge APP (3MB No OTA/1MB SPIFFS)**。

**Hook 不触发**
- Hooks 在会话启动时加载，改完 `settings.json` 必须**完全重启** Claude Code。
- 看 `~/.claude/hooks/claude-display.log` —— 每次 hook 触发都会写一行。
- 校验 `~/.claude/settings.json` 是合法 JSON：`python3 -c "import json,sys; json.load(open(sys.argv[1]))" ~/.claude/settings.json`

**权限弹窗时屏幕不闪 WAITING**
两种可能：
1. Claude Code 版本低于 2.0 —— 还没有 `PermissionRequest` 事件。升级，或者退而求其次手工接 `Notification → waiting`（代价是非许可类通知也会乱闪）。
2. `~/.claude/settings.json` 里缺 `PermissionRequest` 那条 —— 重新合并 [`examples/settings.json`](examples/settings.json)，然后**完全重启** Claude Code（hooks 只在会话启动时加载）。

**屏幕在长任务中途莫名其妙跳到时钟**
某个长时间运行的工具（build、install、慢 ssh 等）连续 5 分钟没发事件，固件认为 session 死了就回退到 IDLE。下一个事件会拉回来。如果嫌太激进，把 [`firmware/claude_status/claude_status.ino`](firmware/claude_status/claude_status.ino) 里的 `WORKING_TIMEOUT_MS` 调大重新烧录。

## 项目结构

```
claude-code-display/
├── README.md / README.zh.md
├── CLAUDE.md                               # Claude Code 打开仓库时自动读的项目提示
├── SKILL.md                                # Claude Code 装 Mac 端时跟着走的步骤指引
├── LICENSE                                 # MIT
├── firmware/
│   └── claude_status/
│       ├── claude_status.ino               # 主固件（注释全英文）
│       └── config.h.example                # 复制为 config.h 后改值
├── hooks/
│   ├── claude-display.sh                   # bash hook（macOS / Linux / Git Bash / WSL）
│   └── claude-display.ps1                  # PowerShell 端口（原生 Windows）
├── examples/
│   ├── settings.json                       # 合并到 ~/.claude/settings.json 的 hooks 配置
│   └── claude-display.conf.example         # 设备 URL / 日志路径覆写（.sh 和 .ps1 共用）
└── docs/
    ├── images/                             # README 用的实拍 / 硬件图
    └── videos/                             # README 演示视频
```

## 许可

MIT —— 见 [LICENSE](LICENSE)。

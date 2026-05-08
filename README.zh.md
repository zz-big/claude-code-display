# Claude Code Display

> 一块 WiFi 连网的小 OLED 屏，告诉你 Claude Code 啥时候需要你看一眼 —— 不用再死盯终端。

[English →](README.md)

<p align="center">
  <img src="docs/images/oled-running.jpg" alt="OLED 实物，WORKING 状态" width="420">
</p>

<sub>`WORKING` 状态实拍。[演示视频](docs/videos/demo.mp4)。</sub>

## 前置条件

- **Claude Code ≥ 2.0** —— WAITING 闪烁需要 2.0 引入的 `PermissionRequest` hook 事件。
- **macOS / Linux / Windows** 都行。[`hooks/`](hooks/) 下两个等价实现：`claude-display.sh`（bash，也能在 Git Bash / WSL 跑）和 `claude-display.ps1`（原生 Windows PowerShell 5.1+），二选一。
- **ESP32-C3 Super Mini**（或任意带 2.4 GHz WiFi 的 ESP32）+ **0.96" SSD1306 OLED** + 2.4 GHz 网络。
- 工具：固件用 `arduino-cli`，主机 hook 用 `curl`（bash 版还建议有 `jq`）。

## Claude Code 用户 —— 一行命令装好

```bash
git clone https://github.com/zz-big/claude-code-display.git
cd claude-code-display
claude
```

然后说一句*"帮我装一下"*。Claude 会跟着 [`SKILL.md`](SKILL.md) 走完整套：烧固件（按需，先把 ESP32 插上）+ 主机配置。已经烧好了直接说一声跳过。手动步骤见下。

## 状态

| 状态        | 视觉效果                          | 触发时机                                  |
|-------------|-----------------------------------|------------------------------------------|
| **WAITING** | 整屏反白闪烁 @ 400ms              | Claude 在请求工具许可                     |
| **WORKING** | 旋转点 + 工具名                   | 正在跑工具（`Bash`、`Edit foo.py` 等）    |
| **DONE**    | 对勾 + "took 14s"                 | 一轮回复结束                              |
| **ERROR**   | X 图标 + 急速反白闪烁             | 工具调用返回错误                          |
| **IDLE**    | 时钟 + 今日统计                   | 没事干                                    |

多个 Claude Code 并发会轮播，**WAITING 永远优先**。

## 接线

| OLED 模块 | ESP32-C3 Super Mini 引脚图 |
|---|---|
| <img src="docs/images/oled-module.jpg" width="280"> | <img src="docs/images/esp32-c3-pinout.jpg" width="280"> |

| OLED  | ESP32-C3            |
|-------|---------------------|
| GND   | GND                 |
| VCC   | 3V3                 |
| SCL   | GPIO 9              |
| SDA   | GPIO 8              |

## 快速上手

### 1 · 烧录固件

Arduino IDE 里装 **U8g2**、**ArduinoJson** 库 + **esp32** 板子包（Espressif），然后：

```bash
cp firmware/claude_status/config.h.example firmware/claude_status/config.h
# 编辑 config.h 填 WiFi 和时区偏移
```

打开 `claude_status.ino`，板型选 **ESP32C3 Dev Module**、分区方案选 **Huge APP**，上传。烧好后 OLED 显示 IP，mDNS 名 `http://claude-display.local` 也能访问。

### 2 · 安装 hook 脚本

```bash
# macOS / Linux / Git Bash / WSL
mkdir -p ~/.claude/hooks
cp hooks/claude-display.sh ~/.claude/hooks/ && chmod +x ~/.claude/hooks/claude-display.sh

# 原生 Windows PowerShell
# $d = Join-Path $HOME '.claude/hooks'; New-Item -ItemType Directory -Path $d -Force | Out-Null
# Copy-Item hooks/claude-display.ps1 $d
```

`claude-display.local` 解析不通（公司 / 客网常屏 mDNS），就用 OLED 启动时显示的 IP：
```bash
echo 'CLAUDE_DISPLAY_URL=http://192.168.x.x/status' > ~/.claude/hooks/claude-display.conf
```

### 3 · 配 Claude Code hooks

把 [`examples/settings.json`](examples/settings.json) 的 `hooks` 块合并到 `~/.claude/settings.json`。然后**完全退出并重启 Claude Code** —— hooks 在会话启动时加载一次。

### 4 · 测试

Claude Code 里随便发条消息，屏幕应该切到 WORKING。手动触发一次 WAITING：

```bash
curl -X POST http://claude-display.local/status -H "Content-Type: application/json" \
  -d '{"state":"waiting","msg":"test","session":"demo"}'
curl -X POST http://claude-display.local/clear   # 清理
```

## 工作原理

每个 Claude Code 事件触发 hook 脚本，hook 把 JSON 推到 ESP32：

| Claude Code 事件 | OLED 状态 |
|---|---|
| `SessionStart` / `SessionEnd` | IDLE |
| `UserPromptSubmit` / `PreToolUse` / `PostToolUse` | WORKING（工具失败为 ERROR） |
| `PermissionRequest` | **WAITING**（反白闪烁——要你处理） |
| `Stop` | DONE |

Hook 是 fire-and-forget、1 秒超时，设备掉线不卡 Claude Code。固件 5 分钟没事件就回退 IDLE。

## 配置项

- **固件**（[`config.h`](firmware/claude_status/config.h.example)）：`WIFI_SSID`、`WIFI_PASSWORD`、`MDNS_NAME`、`TZ_OFFSET_SEC`。
- **Hook**（`~/.claude/hooks/claude-display.conf`，[模板](examples/claude-display.conf.example)）：`CLAUDE_DISPLAY_URL`、`CLAUDE_DISPLAY_LOG`、`CLAUDE_DISPLAY_LOG_MAX_LINES`。三者也支持环境变量覆盖。
- **HTTP API**：`POST /status` `{state,msg,project,session}` · `GET /status` · `POST /clear`。状态：`idle` · `working` · `waiting` · `done` · `error`。
- **安全**：无鉴权，同 LAN 的人都能改屏。别把设备暴露到公网。

## 打赏

请我喝杯咖啡 ☕️ —— 项目本身永远是 MIT。

<p>
  <img src="docs/images/sponsor-wechat.jpg" alt="微信收款码" width="220">
  &nbsp;&nbsp;&nbsp;
  <img src="docs/images/sponsor-alipay.jpg" alt="支付宝收款码" width="220">
</p>

## 许可

MIT —— 见 [LICENSE](LICENSE)。

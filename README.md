# Claude Code Display

> A small WiFi-connected OLED display that tells you when Claude Code needs your attention — so you don't have to keep your eyes glued to the terminal.

[中文文档 →](README.zh.md)

<p align="center">
  <img src="docs/images/oled-running.jpg" alt="OLED running, showing WORKING state" width="420">
</p>

<sub>`WORKING` state, mid-edit. [Demo video](docs/videos/demo.mp4).</sub>

## Requirements

- **Claude Code ≥ 2.0** — needs the `PermissionRequest` hook event for the WAITING flash.
- **macOS, Linux, or Windows.** Two equivalent host hooks ship in [`hooks/`](hooks/): `claude-display.sh` (bash — also runs on Git Bash / WSL) and `claude-display.ps1` (native Windows PowerShell 5.1+). Pick one.
- **ESP32-C3 Super Mini** (or any ESP32 with 2.4 GHz WiFi) + **0.96" SSD1306 OLED** + a 2.4 GHz network.
- Tools: `arduino-cli` for firmware, `curl` for host hook (bash version also benefits from `jq`).

## For Claude Code users — one-shot install

```bash
git clone https://github.com/zz-big/claude-code-display.git
cd claude-code-display
claude
```

Then say *"install this for me"*. Claude follows [`SKILL.md`](SKILL.md) end to end: firmware flash (opt-in — plug in the ESP32 first) and host wiring. Already flashed? Just say so and Claude skips the firmware half. Manual setup steps below.

## States

| State       | Visual                                  | When                                            |
|-------------|-----------------------------------------|-------------------------------------------------|
| **WAITING** | full-screen inverted flash @ 400ms      | Claude is asking permission to use a tool       |
| **WORKING** | spinning dot icon + tool name           | a tool is running (`Bash`, `Edit foo.py`, ...)  |
| **DONE**    | checkmark + "took 14s"                  | a turn finished                                 |
| **ERROR**   | X icon, fast inverted flash             | a tool call returned an error                   |
| **IDLE**    | clock + today's stats                   | nothing happening                               |

Concurrent sessions rotate on screen, with WAITING always taking priority.

## Wiring

| OLED module | ESP32-C3 Super Mini pinout |
|---|---|
| <img src="docs/images/oled-module.jpg" width="280"> | <img src="docs/images/esp32-c3-pinout.jpg" width="280"> |

| OLED  | ESP32-C3            |
|-------|---------------------|
| GND   | GND                 |
| VCC   | 3V3                 |
| SCL   | GPIO 9              |
| SDA   | GPIO 8              |

## Quick start

### 1 · Flash the firmware

In Arduino IDE, install the **U8g2** and **ArduinoJson** libraries plus the **esp32** board package (by Espressif). Then:

```bash
cp firmware/claude_status/config.h.example firmware/claude_status/config.h
# Edit config.h with your WiFi creds + timezone offset.
```

Open `claude_status.ino`, select board **ESP32C3 Dev Module** + partition **Huge APP**, and upload. After boot the OLED shows the IP; the device is also reachable at `http://claude-display.local` via mDNS.

### 2 · Install the hook

```bash
# macOS / Linux / Git Bash / WSL
mkdir -p ~/.claude/hooks
cp hooks/claude-display.sh ~/.claude/hooks/ && chmod +x ~/.claude/hooks/claude-display.sh

# Native Windows PowerShell
# $d = Join-Path $HOME '.claude/hooks'; New-Item -ItemType Directory -Path $d -Force | Out-Null
# Copy-Item hooks/claude-display.ps1 $d
```

If `claude-display.local` doesn't resolve (corporate / guest WiFi often blocks mDNS), pin the IP shown on the OLED at boot:
```bash
echo 'CLAUDE_DISPLAY_URL=http://192.168.x.x/status' > ~/.claude/hooks/claude-display.conf
```

### 3 · Wire up Claude Code hooks

Merge the `hooks` block from [`examples/settings.json`](examples/settings.json) into `~/.claude/settings.json`. Then **fully quit and relaunch Claude Code** — hooks load at session start.

### 4 · Test

Send any prompt in Claude Code — the screen should switch to WORKING. Force a WAITING flash:

```bash
curl -X POST http://claude-display.local/status -H "Content-Type: application/json" \
  -d '{"state":"waiting","msg":"test","session":"demo"}'
curl -X POST http://claude-display.local/clear   # cleanup
```

## How it works

Each Claude Code event fires the hook script, which posts JSON to the ESP32:

| Claude Code event   | OLED state |
|---------------------|------------|
| `SessionStart` / `SessionEnd` | IDLE |
| `UserPromptSubmit` / `PreToolUse` / `PostToolUse` | WORKING (or ERROR on tool failure) |
| `PermissionRequest` | **WAITING** (flashes — needs your attention) |
| `Stop`              | DONE |

The hook is fire-and-forget with a 1-second timeout, so an offline display never blocks Claude Code. Firmware auto-reverts WORKING → IDLE after 5 min of silence.

## Configuration

- **Firmware** ([`config.h`](firmware/claude_status/config.h.example)): `WIFI_SSID`, `WIFI_PASSWORD`, `MDNS_NAME`, `TZ_OFFSET_SEC`.
- **Hook** (`~/.claude/hooks/claude-display.conf`, see [example](examples/claude-display.conf.example)): `CLAUDE_DISPLAY_URL`, `CLAUDE_DISPLAY_LOG`, `CLAUDE_DISPLAY_LOG_MAX_LINES`. All also accept env-var overrides.
- **HTTP API**: `POST /status` `{state,msg,project,session}` · `GET /status` · `POST /clear`. States: `idle` · `working` · `waiting` · `done` · `error`.
- **Security**: no auth — anyone on the LAN can change the screen. Don't expose to the public internet.

## Support

Buy me a coffee ☕️ — project stays MIT either way.

<p>
  <img src="docs/images/sponsor-wechat.jpg" alt="WeChat Pay" width="220">
  &nbsp;&nbsp;&nbsp;
  <img src="docs/images/sponsor-alipay.jpg" alt="Alipay" width="220">
</p>

## License

MIT — see [LICENSE](LICENSE).

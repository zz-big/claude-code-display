---
name: claude-code-display
description: Install and configure the Claude Code Display end-to-end — both flashing the ESP32 firmware (libraries, config.h, compile, upload) and wiring up the Mac/Linux host side (hook script + settings.json). Use when the user has cloned this repo and wants the display set up. The firmware-flash half is opt-in (asks the user before touching their hardware).
---

# Installing Claude Code Display

This skill walks the user from a freshly cloned repo to a working display.

It has two halves:

- **Firmware flash (Part A — optional, opt-in)**: install Arduino libraries via `arduino-cli`, fill in `config.h` with the user's WiFi, detect the serial port, compile + upload.
- **Host wiring (Part B — required)**: copy the hook script to `~/.claude/hooks/`, write a config file with the device URL, merge hooks into `~/.claude/settings.json`, validate connectivity.

## When to use

The user cloned this repo and wants the display set up. Typical phrasings: *"install this"*, *"set this up"*, *"装一下"*, *"帮我配置"*.

## Decide which parts to run

**Detect first, ask only what you can't detect.** Run these silently and gather context before saying anything:

```bash
# Is the device already flashed and reachable on the LAN?
curl -s -m 3 http://claude-display.local/status

# Is an ESP32 plugged in?
"$CLI" board list 2>/dev/null   # see Part A1 for $CLI

# What WiFi is the host currently on? (macOS)
networksetup -getairportnetwork en0 2>/dev/null
# Linux equivalent:
# nmcli -t -f active,ssid dev wifi | awk -F: '$1=="yes"{print $2}'
# or iwgetid -r

# System timezone offset, as seconds — for TZ_OFFSET_SEC.
TZ_HHMM="$(date +%z)"      # e.g. +0800
TZ_OFFSET_SEC=$(( (${TZ_HHMM:0:1}1) * (10#${TZ_HHMM:1:2} * 3600 + 10#${TZ_HHMM:3:2} * 60) ))
```

Then decide:

- **Device responds** (HTTP 200 with `count`/`sessions` JSON) → skip Part A, jump to Part B.
- **Device doesn't respond, no ESP32 plugged in** → tell the user "I don't see a device. Either plug your ESP32 in via USB so I can flash it, or tell me to skip flashing and only configure the host side." Wait for their answer.
- **Device doesn't respond, ESP32 IS plugged in** → present a single confirmation with the auto-detected values, then proceed:

  > I found an ESP32 on `/dev/cu.usbmodem101` and you're currently connected to WiFi `<SSID>` on this Mac. Timezone looks like UTC+8.
  >
  > Want me to flash the firmware with these defaults? I'll need your WiFi password (it goes into `config.h` which is gitignored, never logged, never echoed).
  >
  > Or say "skip flashing" / "I'll flash manually" and I'll only do the host-side configuration.

Never start Part A without explicit user consent for that prompt — flashing modifies hardware and we're handling credentials. But everything you can detect (port, SSID, timezone) should already be in the prompt as defaults, not asked one-by-one.

# Part A — Firmware flash

### A1. Locate `arduino-cli`

Try in order, use the first that exists:

```bash
# 1. System install (recommended — `brew install arduino-cli` on Mac).
which arduino-cli

# 2. Bundled inside Arduino IDE 2.x on Mac.
ls "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"

# 3. Bundled on Linux (path may vary by distro).
ls /opt/Arduino*/resources/app/lib/backend/resources/arduino-cli 2>/dev/null
```

Save the path as `$CLI` for the rest of Part A. If none of these work, tell the user:

> No `arduino-cli` found. Install with `brew install arduino-cli` (Mac) or download from https://arduino.github.io/arduino-cli/. Then re-run.

### A2. Install ESP32 board package + libraries (idempotent)

```bash
"$CLI" core update-index 2>&1 | tail -2
"$CLI" core install esp32:esp32
"$CLI" lib install "U8g2"
"$CLI" lib install "ArduinoJson"
```

These commands are safe to re-run — already-installed packages are no-ops. If the user's Arduino library directory is somewhere unusual (e.g. `~/Documents/Arduino` on Mac, which is TCC-protected), arduino-cli generally still works because it owns its own user-dir at `~/Library/Arduino15/`. Watch for "permission denied" though — if it happens, ask the user to grant Terminal access to Documents in **System Settings → Privacy & Security → Files and Folders**.

### A3. Write `config.h` (auto-detected SSID + timezone, asked-for password)

By the time you reach this step you should already have these from the detection done in "Decide which parts to run":
- `WIFI_SSID` from `networksetup -getairportnetwork en0` (macOS) / `nmcli` / `iwgetid`
- `TZ_OFFSET_SEC` computed from `date +%z`
- `MDNS_NAME` defaulted to `claude-display`

The only value left to gather is `WIFI_PASSWORD` — **always ask the user**. Never grep keychain, login profiles, env vars, or any other config to discover it.

If any of the auto-detections failed (Ethernet-only host, Linux without NetworkManager, weird `date +%z` output), ask just for the missing value — don't ask for things you already detected.

**Strict rules around the password**:

- **Never log the password** anywhere — not into a tracked file, not the hook log, not back into chat. After writing it to `config.h`, refer to it only as `<wifi password>`.
- **Never put the password on a shell command line** (`echo "$PASS" >> file` will leak it via process listings). Use the `Edit` tool to write directly to the file.
- **Never commit `config.h`** — it's gitignored, but verify before any subsequent `git add` (only stage specific files, never `git add .`).
- **Auto mode does NOT authorize credential exposure.** Always ask, even when otherwise running unattended.

Then:

```bash
cp firmware/claude_status/config.h.example firmware/claude_status/config.h
```

`Edit` `firmware/claude_status/config.h` and set the four values. After editing, `Read` the file once to confirm the `YOUR_WIFI_*` placeholders are gone — but **do not paste the file contents into chat**, just confirm "config.h written" to the user.

### A4. Confirm the serial port

You already detected this in the "Decide which parts to run" step via `arduino-cli board list`. If exactly one ESP32 was found, use it. If multiple were found, ask the user to pick. If the FQBN auto-detected as something other than `esp32:esp32:esp32c3` and the user hasn't said otherwise, default to `esp32:esp32:esp32c3` (this repo's reference hardware) and tell the user what you're using.

If multiple ESP32 boards are listed, ask the user which to use. If none are listed, tell them to plug in the device and re-run.

If `board list` shows the port but doesn't auto-detect the FQBN, default to `esp32:esp32:esp32c3` (this repo's reference hardware). Only override if the user says they're using a different board.

### A5. Compile and upload

The default ESP32-C3 partition (1.3MB app) is too small for the GB2312 Chinese font. Always use `huge_app`:

```bash
"$CLI" compile \
  --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" \
  --upload --port "<port>" \
  firmware/claude_status
```

Watch for:
- **"Sketch too big"** → partition flag was missed. Re-run with `:PartitionScheme=huge_app`.
- **"could not open port"** → wrong port, or another program (Serial Monitor, etc.) has it open. Close other tools and retry.
- **"timed out waiting for packet"** → the board didn't enter download mode. On ESP32-C3 Super Mini, hold the `BOOT` button while plugging USB. Or just retry — usually works on the second try.

After successful upload, wait ~10 seconds for the device to boot and join WiFi:

```bash
sleep 12
curl -s -m 3 http://claude-display.local/status
```

If that responds with JSON, Part A is done. If it doesn't, ask the user what's on the OLED (look for "WiFi FAIL" or an IP address). If the OLED shows an IP, capture it for Part B. If the OLED is blank or shows "WiFi FAIL", their credentials are wrong — ask again.

# Part B — Host wiring

## Prerequisites checklist (Part B alone)

If running Part B without A, verify:

1. The user is in the cloned repo (current dir contains `hooks/`, `examples/`, `README.md`).
2. The device is flashed and on the LAN.
3. The device is reachable (`curl -m 3 http://claude-display.local/status` or via IP).

### B1. Locate the repo

Confirm the current working directory is the cloned repo. The repo root must contain `hooks/claude-display.sh`, `examples/settings.json`, and `README.md`. If not, ask the user where they cloned it. Use that path for all subsequent steps. Don't proceed if you can't find these files — the user may have run from the wrong directory.

### B2. Verify the device is reachable & decide on the URL

Always test with `curl` (not `ping`) — they use different resolvers. On macOS especially, `ping claude-display.local` works (uses Bonjour directly) but `curl http://claude-display.local` may NOT, because curl uses `getaddrinfo()` which doesn't always consult mDNS for `.local` names. The hook script uses curl, so curl is what matters.

```bash
# Test mDNS via curl (the same path the hook uses).
curl -s -m 3 http://claude-display.local/status
```

A successful response is a JSON object containing `count` and `sessions`. If this curl hangs or returns nothing (HTTP 000):

```bash
# Ask the user: what IP is shown on the OLED's bottom row at boot?
# Then test:
curl -s -m 3 http://<ip>/status
```

If both fail:
- Verify they're on the same WiFi network as the device.
- Check `arp -a` on Mac for devices on the LAN.
- `ping <ip>` to confirm basic reachability.

Don't proceed past this step until **the curl test** succeeds — `ping` working is not enough.

### B3. Write the device URL to the hook config

The hook script reads `~/.claude/hooks/claude-display.conf` at runtime. Setting `CLAUDE_DISPLAY_URL` there is more reliable than an env var in `~/.zshrc` (Claude Code spawns hooks as non-login subprocesses, which don't necessarily inherit shell rc env).

```bash
mkdir -p ~/.claude/hooks
```

- **mDNS works** → no config file needed; the script falls back to `http://claude-display.local/status`. Skip writing the conf.
- **mDNS fails, IP works** → write the conf:
  ```bash
  cat > ~/.claude/hooks/claude-display.conf <<'EOF'
  CLAUDE_DISPLAY_URL=http://192.168.X.X/status
  EOF
  ```
  Replace `192.168.X.X` with the user's actual IP. Use `Write` rather than appending — this file is owned by us.

### B4. Install the hook script

```bash
mkdir -p ~/.claude/hooks
cp hooks/claude-display.sh ~/.claude/hooks/
chmod +x ~/.claude/hooks/claude-display.sh
```

Verify by running it manually:

```bash
~/.claude/hooks/claude-display.sh waiting "test from skill install"
sleep 1
tail -2 ~/.claude/hooks/claude-display.log
```

The log should show `state=waiting` and the device should respond `{"ok":true}` with HTTP 200. The OLED should briefly flash WAITING. Then clean up:

```bash
curl -X POST http://claude-display.local/clear   # or http://<ip>/clear
```

### B5. Merge hook config into `~/.claude/settings.json`

This is the **critical** step. The user almost certainly has other settings (`model`, `permissions`, etc.) in `~/.claude/settings.json` that you must preserve.

Algorithm:

1. **Read** `~/.claude/settings.json`. If it doesn't exist, create with `{}`.
2. **Parse** as JSON. If invalid, ask the user to fix or back up before proceeding — do not overwrite.
3. **Read** `examples/settings.json` from the repo for the hooks block.
4. **Merge**: copy each hook event (`SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `Notification`, `Stop`, `SessionEnd`) into `settings.json["hooks"]`.
   - If `settings.json["hooks"]` doesn't exist, create it.
   - If a hook event already has entries, **append** the new entry rather than replacing — the user may have other hooks for the same event.
5. **Write back**, preserving 2-space indentation.
6. **Validate**: `python3 -c "import json,sys; json.load(open(sys.argv[1]))" ~/.claude/settings.json`

Use the `Edit` tool for surgical changes. Do not use `Write` to clobber the whole file. If the existing settings.json is structurally complex (e.g. has comments — JSON5 — or unusual nesting), describe what you'd change and ask the user to confirm before writing.

**Self-modification protection**: Claude Code's sandbox may block writes to `~/.claude/settings.json`. If you hit a permission error, print the exact JSON block the user needs to merge in manually and stop — don't try workarounds.

### B6. Final test

```bash
# 1. From the user's shell (so env vars apply):
~/.claude/hooks/claude-display.sh working "test working state"
sleep 1
tail -3 ~/.claude/hooks/claude-display.log

# 2. Clean up:
curl -X POST http://claude-display.local/clear
```

Then tell the user:

> Setup complete. **Fully quit Claude Code (⌘Q on Mac) and reopen it** — hooks load only at session start, they won't take effect in the current session. After restart, send any prompt and the OLED should switch to WORKING.

## Things to NOT do

- **Never start Part A without explicit user consent.** Flashing modifies their hardware and we'll handle their WiFi password — both require informed buy-in.
- **Never log or echo the WiFi password.** Not into `claude-display.log`, not into a tool result, not back into chat. Reference it as `<wifi password>` after `Edit`-ing it into `config.h`. Auto mode does NOT authorize credential exposure.
- **Never commit `config.h`.** It's gitignored, but verify before any subsequent `git add .` — only stage specific files.
- Do not assume mDNS works on every network. Corporate WiFi, guest WiFi, and some routers block multicast DNS.
- Do not modify `~/.claude/settings.json` without backing it up first if it's non-empty: `cp ~/.claude/settings.json ~/.claude/settings.json.bak`.
- Do not silently ignore errors from `arduino-cli`, `curl`, `jq`, or JSON parsing — report them to the user so they can debug.
- Do not assume the script's default `claude-display.local` URL works for everyone. Always test or ask.
- Do not skip the `:PartitionScheme=huge_app` flag on ESP32-C3 — without it, the GB2312 Chinese font won't fit and compile fails.

## Useful debugging commands

```bash
# Watch hook events in real time
tail -f ~/.claude/hooks/claude-display.log

# What does the device think is going on?
curl -s http://claude-display.local/status | python3 -m json.tool

# Drop all sessions (no effect on stats)
curl -X POST http://claude-display.local/clear

# Manually trigger each state for visual verification
for s in working waiting done error; do
  ~/.claude/hooks/claude-display.sh "$s" "manual $s test"
  sleep 3
done
~/.claude/hooks/claude-display.sh idle
```

## How the system works (for context)

```
Claude Code event     →   ~/.claude/hooks/claude-display.sh   →   curl POST   →   ESP32 HTTP server  →  OLED
(UserPromptSubmit,        (extracts session_id, tool, msg)        (1s timeout,     (per-session state    (priority: WAITING > ERROR
 PreToolUse, Stop, ...)                                            backgrounded)    machine + clock)      > WORKING > recent DONE)
```

The hook script is fire-and-forget with a 1-second curl timeout — an offline display can never block Claude Code. The firmware tracks up to 4 concurrent sessions (each `session_id` is a separate slot) and rotates the display every 5 seconds among same-priority sessions. WAITING locks the screen until that session transitions to a different state via the next hook event.

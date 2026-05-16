---
name: claude-code-display
description: Install and configure the Claude Code Display end-to-end — both flashing the ESP32 firmware (libraries, compile, upload; WiFi is entered later via the device's captive portal) and wiring up the host side (hook script + settings.json) on macOS, Linux, or Windows (bash or native PowerShell). Use when the user has cloned this repo and wants the display set up. The firmware-flash half is opt-in (asks the user before touching their hardware).
---

# Installing Claude Code Display

This skill walks the user from a freshly cloned repo to a working display.

It has two halves:

- **Firmware flash (Part A — optional, opt-in)**: install Arduino libraries via `arduino-cli`, detect the serial port, compile + upload. WiFi is configured on the device itself via a captive portal after first boot — no credentials to collect beforehand.
- **Host wiring (Part B — required)**: copy the hook script to `~/.claude/hooks/`, write a config file with the device URL, merge hooks into `~/.claude/settings.json`, validate connectivity.

## When to use

The user cloned this repo and wants the display set up. Typical phrasings: *"install this"*, *"set this up"*, *"装一下"*, *"帮我配置"*.

## Prerequisites

- **Claude Code ≥ 2.0** (uses the `PermissionRequest` hook event for permission popups). On older versions only WORKING / DONE / IDLE will work — WAITING flash on permission popups requires 2.0+. Check with `claude --version` if unsure.
- **macOS, Linux, or Windows** for Part B. Two interchangeable hook implementations ship in [`hooks/`](hooks/):
  - **`claude-display.sh`** (bash) — used on macOS / Linux natively, and on Windows under Git Bash or WSL. Detect with `uname`: `Darwin`, `Linux`, or `MINGW*`/`MSYS*` → use the .sh.
  - **`claude-display.ps1`** (PowerShell 5.1+) — used on **native Windows** when the user doesn't have Git Bash or doesn't want to install it. Detect by `$PSVersionTable` if running in PowerShell, or by checking `$env:OS -eq 'Windows_NT'` along with absence of bash.
  Pick **one** based on the user's shell — don't install both. They share the same `~/.claude/hooks/claude-display.conf` and produce the same JSON payload, so the device side is identical.
- **`jq`** is recommended for the bash version (cleaner payload handling; falls back to hand-built JSON if missing). The PS version uses native `ConvertFrom-Json` and doesn't need jq. Git Bash doesn't ship jq — instruct Windows users on Git Bash to grab it from https://jqlang.org/download/ or `winget install jqlang.jq`.
- For Part A: an **ESP32-C3 Super Mini** (or any ESP32 with 2.4 GHz WiFi) wired to a 0.96" SSD1306 OLED, plus `arduino-cli` (`brew install arduino-cli` / `winget install ArduinoSA.CLI`) or Arduino IDE.
- A **2.4 GHz WiFi network** the ESP32 can reach (ESP32-C3 has no 5 GHz support).

**Windows path note**: under Git Bash, `~/.claude/hooks/` resolves to `C:\Users\<name>\.claude\hooks\`. Claude Code on Windows looks for hooks at the same `~/.claude/hooks/` path — so just use `~` in `settings.json` as on Mac/Linux. Don't hard-code Windows-style paths.

## Step 0 — Detect shell platform

Before running anything, figure out which shell you're in. **Every command block below has two forms — pick one and stick with it.**

Probe (run one of these — whichever your shell understands):

```bash
# In bash (macOS / Linux / Git Bash / WSL):
uname    # Darwin | Linux | MINGW*_NT-* | MSYS*_NT-*
```

```powershell
# In PowerShell (native Windows):
$PSVersionTable.PSVersion   # Major >= 5 means we're good
```

If `uname` succeeds → **PLATFORM = bash**. Use the `claude-display.sh` hook and bash code blocks throughout.
If `$PSVersionTable` is populated and `uname` is unavailable → **PLATFORM = ps**. Use the `claude-display.ps1` hook and the PowerShell code blocks.

For brevity, the rest of this guide labels dual code blocks **Bash:** and **PowerShell:**. Run only the one that matches PLATFORM. If both are shown side-by-side, they do the same thing.

## Decide which parts to run

**Detect first.** Run these silently and gather context before saying anything:

**Bash:**
```bash
# Is the device already flashed and reachable on the LAN?
curl -s -m 3 http://claude-display.local/status

# Is an ESP32 plugged in?
"$CLI" board list 2>/dev/null   # see Part A1 for $CLI
```

**PowerShell:**
```powershell
curl.exe -s -m 3 http://claude-display.local/status
& $CLI board list 2>$null       # $CLI from A1 below
```

Then decide:

- **Device responds** (HTTP 200 with `count`/`sessions` JSON) → skip Part A, jump to Part B.
- **Device doesn't respond, no ESP32 plugged in** → tell the user "I don't see a device. Either plug your ESP32 in via USB so I can flash it, or tell me to skip flashing and only configure the host side." Wait for their answer.
- **Device doesn't respond, ESP32 IS plugged in** → confirm with the user before flashing:

  > I found an ESP32 on `/dev/cu.usbmodem101`. Ready to flash. WiFi credentials are entered on the device itself via a setup web page after first boot — nothing to provide here.
  >
  > Or say "skip flashing" / "I'll flash manually" and I'll only do the host-side configuration.

Never start Part A without explicit user consent — flashing modifies their hardware.

# Part A — Firmware flash

### A1. Locate `arduino-cli`

Try in order, use the first that exists. Save the path as `$CLI` (bash) or `$CLI` (PowerShell) for the rest of Part A.

**Bash:**
```bash
# 1. System install (recommended — `brew install arduino-cli` on Mac).
which arduino-cli

# 2. Bundled inside Arduino IDE 2.x on Mac.
ls "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"

# 3. Bundled on Linux (path may vary by distro).
ls /opt/Arduino*/resources/app/lib/backend/resources/arduino-cli 2>/dev/null
```

**PowerShell:**
```powershell
# 1. System install (recommended — `winget install ArduinoSA.CLI`).
$CLI = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source

# 2. Bundled inside Arduino IDE 2.x on Windows.
if (-not $CLI) {
  $candidates = @(
    "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
    "$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
  )
  $CLI = ($candidates | Where-Object { Test-Path $_ } | Select-Object -First 1)
}
$CLI   # echo to confirm
```

If none of these work, tell the user:

> No `arduino-cli` found. Install with `brew install arduino-cli` (Mac) / `winget install ArduinoSA.CLI` (Windows), or download from https://arduino.github.io/arduino-cli/. Then re-run.

### A2. Install ESP32 board package + libraries (idempotent)

**Bash:**
```bash
"$CLI" core update-index 2>&1 | tail -2
"$CLI" core install esp32:esp32
"$CLI" lib install "U8g2"
"$CLI" lib install "ArduinoJson"
```

**PowerShell:**
```powershell
& $CLI core update-index 2>&1 | Select-Object -Last 2
& $CLI core install esp32:esp32
& $CLI lib install "U8g2"
& $CLI lib install "ArduinoJson"
```

These commands are safe to re-run — already-installed packages are no-ops. If the user's Arduino library directory is somewhere unusual (e.g. `~/Documents/Arduino` on Mac, which is TCC-protected), arduino-cli generally still works because it owns its own user-dir at `~/Library/Arduino15/` (Mac) or `%LOCALAPPDATA%\Arduino15\` (Windows). Watch for "permission denied" though — on Mac, ask the user to grant Terminal access to Documents in **System Settings → Privacy & Security → Files and Folders**; on Windows, this usually means running PowerShell as Administrator or installing arduino-cli to a user-writable location.

### A3. Confirm the serial port

You already detected this in the "Decide which parts to run" step via `arduino-cli board list`. If exactly one ESP32 was found, use it. If multiple were found, ask the user to pick. If the FQBN auto-detected as something other than `esp32:esp32:esp32c3` and the user hasn't said otherwise, default to `esp32:esp32:esp32c3` (this repo's reference hardware) and tell the user what you're using.

If multiple ESP32 boards are listed, ask the user which to use. If none are listed, tell them to plug in the device and re-run.

If `board list` shows the port but doesn't auto-detect the FQBN, default to `esp32:esp32:esp32c3` (this repo's reference hardware). Only override if the user says they're using a different board.

### A4. Compile and upload

The default ESP32-C3 partition (1.3MB app) is too small for the GB2312 Chinese font. Always use `huge_app`. On Windows the port is `COM3` / `COM4` / etc. (not `/dev/cu.*`).

**Bash:**
```bash
"$CLI" compile \
  --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" \
  --upload --port "<port>" \
  firmware/claude_status
```

**PowerShell:**
```powershell
& $CLI compile `
  --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" `
  --upload --port "<port>" `
  firmware/claude_status
```

Watch for:
- **"Sketch too big"** → partition flag was missed. Re-run with `:PartitionScheme=huge_app`.
- **"could not open port"** → wrong port, or another program (Serial Monitor, etc.) has it open. Close other tools and retry.
- **"timed out waiting for packet"** → the board didn't enter download mode. On ESP32-C3 Super Mini, hold the `BOOT` button while plugging USB. Or just retry — usually works on the second try.

### A5. WiFi setup on the device (captive portal)

The firmware ships with no credentials. On first boot it scans NVS for a saved WiFi network, finds none, and comes up as an open access point named `claude-display-XXXX` (last 4 hex of the MAC). The OLED shows:

```
WiFi setup
Phone -> AP:
claude-display-XXXX
Open in browser:
192.168.4.1
```

Tell the user:

> Look at the OLED — it should show "WiFi setup" with an AP name. On your phone:
> 1. Join the WiFi network `claude-display-XXXX` (open, no password).
> 2. The setup page should pop up automatically (captive portal). If not, open `http://192.168.4.1`.
> 3. Pick your home WiFi, enter the password, set your timezone (hours from UTC, e.g. `8` for China, `-5` for US East), hit **Save and reboot**.
> 4. The device restarts and joins your network. The OLED then shows the IP — that's what Part B needs.

If the captive portal doesn't auto-pop on Android, tell the user to manually visit `http://192.168.4.1` — Android sometimes labels the AP "no internet" and skips the prompt.

Wait for the user to confirm WiFi setup is done before continuing. Then verify the device is reachable:

**Bash:**
```bash
curl -s -m 5 http://claude-display.local/status
```

**PowerShell:**
```powershell
curl.exe -s -m 5 http://claude-display.local/status
```

If mDNS doesn't resolve, ask the user for the IP shown on the OLED and use that for Part B.

# Part B — Host wiring

## Prerequisites checklist (Part B alone)

If running Part B without A, verify:

1. The user is in the cloned repo (current dir contains `hooks/`, `examples/`, `README.md`).
2. The device is flashed and on the LAN.
3. The device is reachable (`curl -m 3 http://claude-display.local/status` or via IP).

### B1. Locate the repo

Confirm the current working directory is the cloned repo. The repo root must contain `hooks/claude-display.sh`, `examples/settings.json`, and `README.md`. If not, ask the user where they cloned it. Use that path for all subsequent steps. Don't proceed if you can't find these files — the user may have run from the wrong directory.

### B2. Verify the device is reachable & decide on the URL

Always test with `curl` (not `ping`) — they use different resolvers. On macOS especially, `ping claude-display.local` works (uses Bonjour directly) but `curl http://claude-display.local` may NOT, because curl uses `getaddrinfo()` which doesn't always consult mDNS for `.local` names. The hook script (`.sh` and `.ps1` both) uses curl, so curl is what matters.

**Bash:**
```bash
# Test mDNS via curl (the same path the hook uses).
curl -s -m 3 http://claude-display.local/status
# If mDNS fails, fall back to IP:
curl -s -m 3 http://<ip>/status
```

**PowerShell:**
```powershell
# curl.exe ships in System32 on Windows 10+ — use it explicitly to avoid
# PowerShell's `curl` alias for Invoke-WebRequest (different flags).
curl.exe -s -m 3 http://claude-display.local/status
curl.exe -s -m 3 http://<ip>/status
```

A successful response is a JSON object containing `count` and `sessions`. If both calls fail:
- Verify they're on the same WiFi network as the device.
- Check `arp -a` (works on both Mac and Windows) for devices on the LAN.
- `ping <ip>` to confirm basic reachability.

Don't proceed past this step until **the curl test** succeeds — `ping` working is not enough.

### B3. Write the device URL to the hook config

The hook script (both `.sh` and `.ps1`) reads `~/.claude/hooks/claude-display.conf` at runtime. Setting `CLAUDE_DISPLAY_URL` there is more reliable than an env var in `~/.zshrc` / user profile (Claude Code spawns hooks as non-login subprocesses, which don't necessarily inherit shell rc env).

**Bash:**
```bash
mkdir -p ~/.claude/hooks
```

**PowerShell:**
```powershell
New-Item -ItemType Directory -Force -Path (Join-Path $HOME '.claude/hooks') | Out-Null
```

- **mDNS works** → no config file needed; the script falls back to `http://claude-display.local/status`. Skip writing the conf.
- **mDNS fails, IP works** → write the conf. Replace `192.168.X.X` with the user's actual IP. Use the `Write` tool (not appending) — this file is owned by us.

  **Bash:**
  ```bash
  cat > ~/.claude/hooks/claude-display.conf <<'EOF'
  CLAUDE_DISPLAY_URL=http://192.168.X.X/status
  EOF
  ```

  **PowerShell:**
  ```powershell
  Set-Content -Path (Join-Path $HOME '.claude/hooks/claude-display.conf') `
              -Value 'CLAUDE_DISPLAY_URL=http://192.168.X.X/status' -Encoding ASCII
  ```

### B4. Install the hook script

**Bash version** (macOS / Linux / Git Bash / WSL):

```bash
mkdir -p ~/.claude/hooks
cp hooks/claude-display.sh ~/.claude/hooks/
chmod +x ~/.claude/hooks/claude-display.sh

~/.claude/hooks/claude-display.sh waiting "test from skill install"
sleep 1
tail -2 ~/.claude/hooks/claude-display.log
```

**PowerShell version** (native Windows):

```powershell
$dir = Join-Path $HOME '.claude/hooks'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
Copy-Item hooks/claude-display.ps1 $dir

pwsh -NoProfile -ExecutionPolicy Bypass -File "$dir/claude-display.ps1" waiting "test from skill install"
Start-Sleep -Seconds 1
Get-Content (Join-Path $HOME '.claude/hooks/claude-display.log') -Tail 2
```

Whichever version you used, the log should show `state=waiting` and the device should respond `{"ok":true}` with HTTP 200. The OLED should briefly flash WAITING. Then clean up (use `curl.exe` on PowerShell):

```bash
curl -X POST http://claude-display.local/clear   # or http://<ip>/clear
```

### B5. Merge hook config into `~/.claude/settings.json`

This is the **critical** step. The user almost certainly has other settings (`model`, `permissions`, etc.) in `~/.claude/settings.json` that you must preserve.

Algorithm:

1. **Read** `~/.claude/settings.json`. If it doesn't exist, create with `{}`.
2. **Parse** as JSON. If invalid, ask the user to fix or back up before proceeding — do not overwrite.
3. **Read** `examples/settings.json` from the repo for the hooks block.
   - On native Windows (PowerShell version), rewrite each `command` string before merging: replace `~/.claude/hooks/claude-display.sh <state>` with `pwsh -NoProfile -ExecutionPolicy Bypass -File "$env:USERPROFILE\.claude\hooks\claude-display.ps1" <state>`. The `<state>` arg (idle/working/waiting/done/postool/idle) and any second-arg message are unchanged.
4. **Merge**: copy each hook event (`SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PermissionRequest`, `Stop`, `SessionEnd`) into `settings.json["hooks"]`.
   - If `settings.json["hooks"]` doesn't exist, create it.
   - If a hook event already has entries, **append** the new entry rather than replacing — the user may have other hooks for the same event.
   - `PermissionRequest` is the event that fires the WAITING flash on tool permission popups. Requires Claude Code ≥ 2.0 — earlier versions used `Notification` for this and won't trigger our hook. If the user is on an old version, install both events as a fallback (the older `Notification` block was: `{ "hooks": [ { "type": "command", "command": "~/.claude/hooks/claude-display.sh waiting" } ] }`).
   - If the user already has a stale install with `Notification → waiting`, **remove that entry** during merge — current Claude Code uses `Notification` for non-permission events too (idle 60s, auth, elicitation), and mapping it all to WAITING causes spurious flashing.
5. **Write back**, preserving 2-space indentation.
6. **Validate**:
   - **Bash:** `python3 -c "import json,sys; json.load(open(sys.argv[1]))" ~/.claude/settings.json`
   - **PowerShell:** `Get-Content (Join-Path $HOME '.claude/settings.json') -Raw | ConvertFrom-Json | Out-Null` (throws if invalid)

Use the `Edit` tool for surgical changes. Do not use `Write` to clobber the whole file. If the existing settings.json is structurally complex (e.g. has comments — JSON5 — or unusual nesting), describe what you'd change and ask the user to confirm before writing.

**Self-modification protection**: Claude Code's sandbox may block writes to `~/.claude/settings.json`. If you hit a permission error, print the exact JSON block the user needs to merge in manually and stop — don't try workarounds.

### B6. Final test

**Bash:**
```bash
~/.claude/hooks/claude-display.sh working "test working state"
sleep 1
tail -3 ~/.claude/hooks/claude-display.log
curl -X POST http://claude-display.local/clear
```

**PowerShell:**
```powershell
pwsh -NoProfile -ExecutionPolicy Bypass `
  -File (Join-Path $HOME '.claude/hooks/claude-display.ps1') working "test working state"
Start-Sleep -Seconds 1
Get-Content (Join-Path $HOME '.claude/hooks/claude-display.log') -Tail 3
curl.exe -X POST http://claude-display.local/clear
```

Then tell the user:

> Setup complete. **Fully quit Claude Code (⌘Q on Mac, or close all windows on Windows) and reopen it** — hooks load only at session start, they won't take effect in the current session. After restart, send any prompt and the OLED should switch to WORKING.

## Things to NOT do

- **Never start Part A without explicit user consent.** Flashing modifies their hardware — it needs informed buy-in.
- **Never ask the user for their WiFi password** in chat. Credentials go in via the device's own captive-portal page directly from their phone — they don't pass through this conversation.
- Do not assume mDNS works on every network. Corporate WiFi, guest WiFi, and some routers block multicast DNS.
- Do not modify `~/.claude/settings.json` without backing it up first if it's non-empty:
  - **Bash:** `cp ~/.claude/settings.json ~/.claude/settings.json.bak`
  - **PowerShell:** `Copy-Item (Join-Path $HOME '.claude/settings.json') (Join-Path $HOME '.claude/settings.json.bak')`
- Do not silently ignore errors from `arduino-cli`, `curl`, `jq`, or JSON parsing — report them to the user so they can debug.
- Do not assume the script's default `claude-display.local` URL works for everyone. Always test or ask.
- Do not skip the `:PartitionScheme=huge_app` flag on ESP32-C3 — without it, the GB2312 Chinese font won't fit and compile fails.

## Useful debugging commands

**Bash:**
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

**PowerShell:**
```powershell
# Watch hook events in real time
Get-Content (Join-Path $HOME '.claude/hooks/claude-display.log') -Wait -Tail 10

# What does the device think is going on?
curl.exe -s http://claude-display.local/status | ConvertFrom-Json | ConvertTo-Json -Depth 5

# Drop all sessions (no effect on stats)
curl.exe -X POST http://claude-display.local/clear

# Manually trigger each state for visual verification
$ps1 = Join-Path $HOME '.claude/hooks/claude-display.ps1'
foreach ($s in 'working','waiting','done','error') {
  pwsh -NoProfile -ExecutionPolicy Bypass -File $ps1 $s "manual $s test"
  Start-Sleep -Seconds 3
}
pwsh -NoProfile -ExecutionPolicy Bypass -File $ps1 idle
```

## How the system works (for context)

```
Claude Code event       →  ~/.claude/hooks/claude-display.sh  →  curl POST    →  ESP32 HTTP server   →  OLED
(SessionStart            )                                       (1s timeout,    (per-session state     (priority: WAITING > ERROR
 UserPromptSubmit  → working  (extracts session_id, tool, msg)    backgrounded)   machine + clock)        > WORKING > recent DONE)
 PreToolUse        → working
 PermissionRequest → waiting
 PostToolUse       → working/error (via "postool" arg)
 Stop              → done
 SessionEnd        → idle)
```

The hook script is fire-and-forget with a 1-second curl timeout — an offline display can never block Claude Code. The firmware tracks up to 4 concurrent sessions (each `session_id` is a separate slot) and rotates the display every 5 seconds among same-priority sessions. WAITING locks the screen until that session transitions to a different state via the next hook event. WORKING auto-reverts to IDLE after 5 minutes of no events (handles user interrupts and missed Stop hooks).

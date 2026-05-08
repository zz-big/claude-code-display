---
name: claude-code-display
description: Install and configure the Claude Code Display host side (hook script + settings.json). Use when the user has cloned this repo and wants the Mac/Linux side wired up so Claude Code events get pushed to their ESP32 OLED. Does NOT flash firmware — that's a separate manual step the user does in Arduino IDE.
---

# Installing Claude Code Display (host side)

This skill configures the Mac/Linux side of the Claude Code Display project — copying the hook script, registering hooks in `~/.claude/settings.json`, and verifying the device is reachable. It does **not** flash firmware to the ESP32 (the user does that themselves in Arduino IDE before running this skill).

## When to use

The user has cloned/downloaded the `claude-code-display` repo and wants the host side installed. Typical phrasings: *"install this"*, *"set this up"*, *"configure my Claude Code to talk to the display"*.

## Prerequisites the user must have done first

1. Cloned this repo somewhere on their machine.
2. Already flashed `firmware/claude_status/claude_status.ino` to the ESP32 (after copying `config.h.example` → `config.h` and editing WiFi credentials).
3. The device is powered on and connected to the same LAN as the user's computer.

If any of these isn't true, **stop and tell the user what's missing** before proceeding — don't try to "fix" it by guessing credentials or skipping checks.

## Step-by-step

### 1. Locate the repo

Confirm the current working directory is the cloned repo. The repo root must contain `hooks/claude-display.sh`, `examples/settings.json`, and `README.md`. If not, ask the user where they cloned it. Use that path for all subsequent steps. Don't proceed if you can't find these files — the user may have run from the wrong directory.

### 2. Verify the device is reachable

Ask the user for the device's IP **or** confirm mDNS resolution works. Try in this order:

```bash
# Try mDNS first (works on most LANs).
curl -s -m 3 http://claude-display.local/status

# If mDNS fails, ask the user for the IP shown on the OLED at boot,
# then test with the IP:
curl -s -m 3 http://<ip>/status
```

Successful response is a JSON object containing `count` and `sessions`. If both fail:
- Verify the user can ping the device (`ping claude-display.local` or `ping <ip>`).
- Verify they're on the same WiFi network as the device.
- Check `arp -a` on Mac to find devices on the LAN.

Don't proceed past this step until reachability is confirmed.

### 3. Decide on the device URL

- mDNS works → use `http://claude-display.local/status` (default in script, no env var needed).
- mDNS fails but IP works → user must `export CLAUDE_DISPLAY_URL=http://<ip>/status` in their shell config (`~/.zshrc` or `~/.bashrc`). Do this with `Edit`, not `>>` — preserve existing shell config.

### 4. Install the hook script

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

### 5. Merge hook config into `~/.claude/settings.json`

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

### 6. Final test

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

- Do not try to flash the firmware — that requires the user's hands on the device, the right partition scheme settings, and the right serial port. Tell them to follow `README.md` § "Quick start → 1. Flash the firmware".
- Do not write the user's WiFi password anywhere we can see — `config.h` is gitignored, but never commit it or display it back to the user beyond what they typed.
- Do not assume mDNS works on every network. Corporate WiFi, guest WiFi, and some routers block multicast DNS.
- Do not modify `~/.claude/settings.json` without backing it up first if it's non-empty: `cp ~/.claude/settings.json ~/.claude/settings.json.bak`.
- Do not silently ignore errors from `curl`, `jq`, or JSON parsing — report them to the user so they can debug.
- Do not assume the script's default `claude-display.local` URL works for everyone. Always ask or test.

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

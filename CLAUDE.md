# Project: Claude Code Display

This is the source repository for **Claude Code Display** — a small ESP32 + OLED hardware status indicator for Claude Code. See `README.md` for the user-facing description.

## When the user opens Claude Code in this repo

Most users clone this repo because they want the display set up on their machine. Common asks: *"install this"*, *"set this up"*, *"help me get this running"*, *"配置一下"*.

When that happens:

1. **Read [`SKILL.md`](SKILL.md)** — it's a step-by-step install guide written for you (Claude). Follow it.
2. SKILL.md has two halves:
   - **Part A (firmware flash, opt-in)** — install Arduino libraries via `arduino-cli`, write `config.h` with the user's WiFi, detect the serial port, compile + upload. Only run this with **explicit user consent** because it touches hardware and handles credentials.
   - **Part B (host wiring, required)** — copy the hook script, write the device URL config, merge hooks into `~/.claude/settings.json`, validate.
3. **Skip Part A** if the device is already flashed and reachable (`curl -s -m 3 http://claude-display.local/status` returns JSON).
4. **WiFi password handling**: ask directly, write to `config.h` via `Edit`, never echo or log it. Auto mode does NOT authorize credential exposure.

If the user just wants to read about the project or modify the code, treat it like any other repo — no special action needed.

## What this repo contains

```
firmware/claude_status/   ESP32 Arduino sketch + config.h template
hooks/claude-display.sh   Bash hook (macOS / Linux / Git Bash / WSL)
hooks/claude-display.ps1  PowerShell port (native Windows)
examples/settings.json    Hooks block to merge into ~/.claude/settings.json
docs/                     README images and demo video
SKILL.md                  Install guide for Claude (the agent)
README.md / README.zh.md  User-facing docs (English / Chinese)
```

## Editing guidelines for contributors (or for Claude when asked to modify)

- **All code comments stay English.** This is an open-source project; non-English comments will get translated back.
- **Never commit `firmware/claude_status/config.h`** — it contains WiFi credentials. The `.gitignore` already excludes it; the template is `config.h.example`.
- **Don't introduce new heavy dependencies on the firmware side.** The current sketch fits in the default ESP32-C3 partition; adding e.g. `WiFiClientSecure` pushes it over and forces users to reconfigure their Arduino IDE.
- **Bash hook must stay POSIX-ish.** `hooks/claude-display.sh` runs on macOS/Linux/Git Bash; don't rely on bash 5+ features or non-portable utilities (no associative arrays, no `mapfile`, no GNU-only flags).
- **Keep the bash and PowerShell hooks in lockstep.** They share the same conf file, env var names, and JSON payload shape. If you change one (new state, new field, log format change, etc.), update the other in the same commit. The PS port targets PowerShell 5.1+ so it works on stock Windows without a pwsh install.
- **Don't break existing HTTP endpoint shapes** without bumping a version somewhere — the hook script and any third-party integrations rely on them.

## Self-modification protection

Claude Code's sandbox typically blocks writes to `~/.claude/settings.json` and `~/.claude/hooks/`. If you hit a permission error during install, **print the exact change the user needs to make and stop** — don't try to work around the block.

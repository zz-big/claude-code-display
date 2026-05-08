<#
claude-display.ps1 — PowerShell port of claude-display.sh.

For Windows users running Claude Code with native PowerShell hooks (no Git Bash
or WSL needed). Functionality mirrors the bash version: same JSON payload shape,
same conf file format, same env vars, so both scripts can coexist.

Usage:
  pwsh -File claude-display.ps1 <state> [message]
    state: idle | working | waiting | done | error | postool

Configuration (priority order):
  1. $env:CLAUDE_DISPLAY_URL
  2. ~/.claude/hooks/claude-display.conf  (KEY=value lines, same format as
     the bash version — both scripts read it the same way)
  3. http://claude-display.local/status   (mDNS default)

Targets PowerShell 5.1+ (the default on Windows 10/11). Uses only cmdlets and
language features available in 5.1, so it should work without installing pwsh.

Hook payload from Claude Code is read from stdin as JSON. Posts a 1-second
synchronous request — even an offline device returns within 1s, comfortably
inside Claude Code's hook timeout budget.
#>

param(
  [string]$State = 'idle',
  [string]$MsgArg = ''
)

$ErrorActionPreference = 'Continue'

# ---------- Resolve home dir (cross-version) ----------
$HomeDir = if ($HOME) { $HOME } else { $env:USERPROFILE }

# ---------- Load conf file (KEY=value, same format as the bash version) ----------
$ConfPath = if ($env:CLAUDE_DISPLAY_CONF) { $env:CLAUDE_DISPLAY_CONF } `
            else { Join-Path $HomeDir '.claude/hooks/claude-display.conf' }

if (Test-Path $ConfPath) {
  foreach ($line in (Get-Content $ConfPath -ErrorAction SilentlyContinue)) {
    $t = $line.Trim()
    if ($t -and -not $t.StartsWith('#') -and ($t -match '^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$')) {
      $k = $matches[1]
      $v = $matches[2].Trim().Trim('"').Trim("'")
      # Env vars take precedence — don't overwrite if already set.
      if (-not (Test-Path "Env:$k")) {
        Set-Item -Path "Env:$k" -Value $v
      }
    }
  }
}

$Url = if ($env:CLAUDE_DISPLAY_URL) { $env:CLAUDE_DISPLAY_URL } `
       else { 'http://claude-display.local/status' }

# ---------- Project name: prefer git repo name, else parent/current ----------
$Project = ''
try {
  $GitTop = & git -C $PWD rev-parse --show-toplevel 2>$null
  if ($LASTEXITCODE -eq 0 -and $GitTop) {
    $Project = Split-Path -Leaf ($GitTop.Trim())
  }
} catch {}
if (-not $Project) {
  $Parent  = Split-Path -Leaf (Split-Path -Parent $PWD)
  $Current = Split-Path -Leaf $PWD
  $Project = "$Parent/$Current"
}

# ---------- Read JSON payload from stdin (Claude Code pipes it in) ----------
$Payload = $null
if ([Console]::IsInputRedirected) {
  try {
    $raw = [Console]::In.ReadToEnd()
    if ($raw) { $Payload = $raw | ConvertFrom-Json -ErrorAction SilentlyContinue }
  } catch {}
}

# ---------- Session id (with 8-char tag for the device) ----------
$SessionId = if ($Payload -and $Payload.session_id) { [string]$Payload.session_id } `
             else { Split-Path -Leaf $PWD }
$SessionTag = if ($SessionId.Length -gt 8) { $SessionId.Substring(0, 8) } else { $SessionId }

# ---------- postool: route to error/working based on tool_response ----------
if ($State -eq 'postool' -and $Payload) {
  $errText = ''
  $isError = $false
  if ($Payload.tool_response) {
    if ($Payload.tool_response.is_error -eq $true) { $isError = $true }
    if ($Payload.tool_response.error -and ([string]$Payload.tool_response.error).Length -gt 0) {
      $isError = $true
      $errText = [string]$Payload.tool_response.error
    } elseif ($isError -and $Payload.tool_response.content `
              -and $Payload.tool_response.content.Count -gt 0 `
              -and $Payload.tool_response.content[0].text) {
      $errText = [string]$Payload.tool_response.content[0].text
    }
  }
  if ($isError) {
    $State = 'error'
    if ($errText) { $MsgArg = $errText }
  } else {
    $State = 'working'
  }
}

# ---------- Build the message ----------
$Msg = $MsgArg
if (-not $Msg -and $Payload) {
  if ($Payload.tool_name) {
    $tool = [string]$Payload.tool_name
    $detail = ''
    if ($Payload.tool_input) {
      foreach ($k in @('command', 'file_path', 'pattern', 'path', 'description')) {
        $prop = $Payload.tool_input.PSObject.Properties[$k]
        if ($prop -and $prop.Value) {
          $detail = [string]$prop.Value
          break
        }
      }
    }
    if ($detail -and ($tool -in @('Read', 'Edit', 'Write', 'NotebookEdit', 'MultiEdit'))) {
      $detail = Split-Path -Leaf $detail
    }
    $Msg = if ($detail) { "${tool}: $detail" } else { $tool }
  } elseif ($Payload.message) {
    $Msg = [string]$Payload.message
  } elseif ($Payload.prompt) {
    $Msg = [string]$Payload.prompt
  }
}

# ---------- Strip ASCII control chars; UTF-8 (Chinese, etc.) passes through ----------
$Msg = $Msg -replace "[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]", ''

# Don't echo the state name itself — firmware already shows it big.
if ($Msg -eq $State) { $Msg = '' }

# Truncate to 60 chars (the OLED scrolls beyond that).
if ($Msg.Length -gt 60) { $Msg = $Msg.Substring(0, 60) }

# ---------- Build the JSON payload ----------
$Json = ([pscustomobject]@{
  state   = $State
  msg     = $Msg
  project = $Project
  session = $SessionTag
}) | ConvertTo-Json -Compress

# ---------- Log + cheap rotation (matches the bash version) ----------
$Log = if ($env:CLAUDE_DISPLAY_LOG) { $env:CLAUDE_DISPLAY_LOG } `
       else { Join-Path $HomeDir '.claude/hooks/claude-display.log' }
$LogMaxLines = if ($env:CLAUDE_DISPLAY_LOG_MAX_LINES) { [int]$env:CLAUDE_DISPLAY_LOG_MAX_LINES } `
               else { 2000 }
$LogDir = Split-Path -Parent $Log
if ($LogDir -and -not (Test-Path $LogDir)) {
  try { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null } catch {}
}
$ts = (Get-Date).ToString('HH:mm:ss')
Add-Content -Path $Log -Value "[$ts] state=$State sid=$SessionTag url=$Url json=$Json" -Encoding UTF8 -ErrorAction SilentlyContinue

# Rotate occasionally (1/50 invocations) so we don't run wc on every event.
if ((Get-Random -Maximum 50) -eq 0) {
  try {
    $lc = (Get-Content $Log -ReadCount 0 -ErrorAction Stop).Count
    if ($lc -gt ($LogMaxLines * 2)) {
      Get-Content $Log -Tail $LogMaxLines | Set-Content "$Log.tmp" -Encoding UTF8
      Move-Item -Force "$Log.tmp" $Log
    }
  } catch {}
}

# ---------- POST to the device (1-second timeout, synchronous) ----------
# Synchronous keeps the script simple and avoids the temp-file dance Start-Process
# would need. 1s worst case is well below Claude Code's hook timeout.
try {
  $resp = Invoke-RestMethod -Uri $Url -Method Post -Body $Json `
            -ContentType 'application/json' -TimeoutSec 1 -ErrorAction Stop
  Add-Content -Path $Log -Value "  -> $($resp | ConvertTo-Json -Compress)" -Encoding UTF8 -ErrorAction SilentlyContinue
} catch {
  Add-Content -Path $Log -Value "  -> error: $($_.Exception.Message)" -Encoding UTF8 -ErrorAction SilentlyContinue
}

exit 0

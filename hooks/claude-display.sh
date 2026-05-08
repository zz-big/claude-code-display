#!/bin/bash
# claude-display.sh — Push Claude Code state to the OLED status display.
#
# Usage:
#   claude-display.sh <state> [message]
#     state: idle | working | waiting | done | error | postool
#
# `postool` is a meta-state used by PostToolUse hooks: the script inspects
# the tool_response payload and dispatches to either `error` or `working`.
#
# Configuration:
#   CLAUDE_DISPLAY_URL  override the device URL.
#                       default: http://claude-display.local/status
#                       set to http://<ip>/status if mDNS doesn't resolve.

set -u

STATE="${1:-idle}"
MSG_ARG="${2:-}"
URL="${CLAUDE_DISPLAY_URL:-http://claude-display.local/status}"

# Project name: prefer the git repo name; otherwise show parent/dir so
# generic basenames like "src" or "test" don't dominate the screen.
if GIT_TOP="$(git -C "$PWD" rev-parse --show-toplevel 2>/dev/null)"; then
  PROJECT="$(basename "$GIT_TOP")"
else
  PARENT="$(basename "$(dirname "$PWD")")"
  CURRENT="$(basename "$PWD")"
  PROJECT="$PARENT/$CURRENT"
fi

# Claude Code pipes the hook payload as JSON on stdin. We mine it for
# session_id, tool name, message, etc.
PAYLOAD=""
if [ ! -t 0 ]; then
  PAYLOAD="$(cat || true)"
fi

# Session id from payload (Claude Code provides it in every hook). Fall back
# to PWD basename if missing — that way pre-payload manual tests still work.
SESSION_ID=""
if [ -n "$PAYLOAD" ] && command -v jq >/dev/null 2>&1; then
  SESSION_ID="$(printf '%s' "$PAYLOAD" | jq -r '.session_id // empty' 2>/dev/null || true)"
fi
[ -z "$SESSION_ID" ] && SESSION_ID="$(basename "$PWD")"
SESSION_TAG="${SESSION_ID:0:8}"

# postool: route to error or working depending on tool_response.is_error.
if [ "$STATE" = "postool" ] && [ -n "$PAYLOAD" ] && command -v jq >/dev/null 2>&1; then
  IS_ERROR="$(printf '%s' "$PAYLOAD" | jq -r '
    (.tool_response.is_error // false) as $a |
    (.tool_response.error // empty) as $b |
    if $a == true or ($b | length) > 0 then "true" else "false" end
  ' 2>/dev/null || echo "false")"

  if [ "$IS_ERROR" = "true" ]; then
    STATE="error"
    ERR_TEXT="$(printf '%s' "$PAYLOAD" | jq -r '
      .tool_response.error //
      (.tool_response.content[0].text // empty)
    ' 2>/dev/null || true)"
    [ -n "$ERR_TEXT" ] && MSG_ARG="$ERR_TEXT"
  else
    STATE="working"
  fi
fi

# Build the message. Order:
#   1. explicit arg from the caller (settings.json)
#   2. tool name + tool input detail (PreToolUse / PostToolUse)
#   3. .message or .prompt fields (Notification)
MSG="$MSG_ARG"
if [ -z "$MSG" ] && [ -n "$PAYLOAD" ] && command -v jq >/dev/null 2>&1; then
  TOOL="$(printf '%s' "$PAYLOAD" | jq -r '.tool_name // empty' 2>/dev/null || true)"
  if [ -n "$TOOL" ]; then
    DETAIL="$(printf '%s' "$PAYLOAD" | jq -r '
      .tool_input | (.command // .file_path // .pattern // .path // .description // empty)
    ' 2>/dev/null || true)"
    case "$TOOL" in
      Read|Edit|Write|NotebookEdit|MultiEdit)
        [ -n "$DETAIL" ] && DETAIL="$(basename "$DETAIL")"
        ;;
    esac
    if [ -n "$DETAIL" ]; then
      MSG="$TOOL: $DETAIL"
    else
      MSG="$TOOL"
    fi
  else
    HOOK_MSG="$(printf '%s' "$PAYLOAD" | jq -r '.message // .prompt // empty' 2>/dev/null || true)"
    [ -n "$HOOK_MSG" ] && MSG="$HOOK_MSG"
  fi
fi

# Strip NULL and ASCII control chars; UTF-8 (Chinese, etc.) passes through.
MSG="$(printf '%s' "$MSG" | LC_ALL=C tr -d '\000-\010\013\014\016-\037\177')"

# Don't echo the state name as the message — the firmware already shows it big.
[ "$MSG" = "$STATE" ] && MSG=""

# Truncate; the OLED can show ~60 chars before scrolling.
MSG="${MSG:0:60}"

# Build the payload JSON (jq if available, hand-built fallback).
if command -v jq >/dev/null 2>&1; then
  JSON="$(jq -nc \
    --arg s "$STATE" \
    --arg m "$MSG" \
    --arg p "$PROJECT" \
    --arg sid "$SESSION_TAG" \
    '{state:$s, msg:$m, project:$p, session:$sid}')"
else
  ESC_MSG="$(printf '%s' "$MSG"     | sed 's/\\/\\\\/g; s/"/\\"/g')"
  ESC_PRJ="$(printf '%s' "$PROJECT" | sed 's/\\/\\\\/g; s/"/\\"/g')"
  ESC_SID="$(printf '%s' "$SESSION_TAG" | sed 's/\\/\\\\/g; s/"/\\"/g')"
  JSON="{\"state\":\"$STATE\",\"msg\":\"$ESC_MSG\",\"project\":\"$ESC_PRJ\",\"session\":\"$ESC_SID\"}"
fi

# Optional log so you can see whether hooks fired (and what the device replied).
LOG="${CLAUDE_DISPLAY_LOG:-$HOME/.claude/hooks/claude-display.log}"
mkdir -p "$(dirname "$LOG")" 2>/dev/null || true
echo "[$(date '+%H:%M:%S')] state=$STATE sid=$SESSION_TAG url=$URL json=$JSON" >> "$LOG"

# Fire-and-forget with a 1 s timeout. An offline display must never block
# Claude Code, so we background the curl and exit immediately.
(
  RESP="$(curl -s -m 1 -w '\nHTTP_CODE=%{http_code}' -X POST "$URL" \
              -H "Content-Type: application/json" -d "$JSON" 2>&1)"
  echo "  -> $RESP" >> "$LOG"
) &

exit 0

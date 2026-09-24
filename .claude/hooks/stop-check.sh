#!/bin/bash
# Enforces the progress protocol: don't finish a turn with uncommitted or unpushed work on a
# milestone branch. Exit 2 sends the message back to Claude. It fires once per stop
# (the stop_hook_active guard prevents loops).
input=$(cat)
active=$(printf '%s' "$input" | python3 -c 'import sys,json
try: print(str(json.load(sys.stdin).get("stop_hook_active", False)).lower())
except Exception: print("false")' 2>/dev/null)
[ "$active" = "true" ] && exit 0

cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
git rev-parse --git-dir >/dev/null 2>&1 || exit 0
branch=$(git branch --show-current)
[ -z "$branch" ] || [ "$branch" = "main" ] && exit 0

dirty=$(git status --porcelain 2>/dev/null)
if git rev-parse --abbrev-ref '@{u}' >/dev/null 2>&1; then
    unpushed=$(git rev-list --count '@{u}..HEAD' 2>/dev/null || echo 0)
else
    unpushed=$(git rev-list --count HEAD --not --remotes 2>/dev/null || echo 1)
fi

if [ -n "$dirty" ] || [ "${unpushed:-0}" -gt 0 ]; then
    {
        echo "Progress protocol (CLAUDE.md): before stopping, update the milestone log and docs/STATUS.md"
        echo "(parallel lanes: just the log), then commit and push."
        [ -n "$dirty" ] && echo "Uncommitted changes:" && echo "$dirty" | head -n 15
        echo "Unpushed commits: ${unpushed:-0}"
    } >&2
    exit 2
fi
exit 0

#!/bin/bash
# Prints the current project state into Claude's context at startup, resume, and after
# compaction, so a fresh session can pick up where the last one stopped.
cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
branch=$(git branch --show-current 2>/dev/null)
echo "=== bongOS session context ==="
echo "Branch: ${branch:-unknown}"
echo "Recent commits:"
git log --oneline -5 2>/dev/null | sed 's/^/  /'
if [[ "$branch" =~ ^m([0-9]+)-([0-9]+)- ]]; then
    log="docs/logs/M${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.md"
    if [ -f "$log" ]; then
        echo; echo "=== $log (last 40 lines) ==="; tail -n 40 "$log"
    fi
fi
if [ -f docs/STATUS.md ]; then
    echo; echo "=== docs/STATUS.md ==="; head -n 80 docs/STATUS.md
fi
echo
echo "Follow the session protocol in CLAUDE.md. Resume from 'Next step' if a milestone is in progress."
exit 0

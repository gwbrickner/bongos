#!/bin/bash
# Prints the current project state into Claude's context at startup, resume, and after
# compaction, so a fresh session can pick up where the last one stopped.
cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
branch=$(git branch --show-current 2>/dev/null)
echo "=== bongOS session context ==="
echo "Branch: ${branch:-unknown}"
git status --porcelain 2>/dev/null | head -n 1 | grep -q . && echo "WARNING: the working tree has uncommitted changes (git status)."
echo "Recent commits:"
git log --oneline -5 2>/dev/null | sed 's/^/  /'
if [[ "$branch" =~ ^m([0-9]+)-([0-9]+)- ]]; then
    log="docs/logs/M${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.md"
    if [ -f "$log" ]; then
        echo; echo "=== $log (last 40 lines) ==="; tail -n 40 "$log"
    fi
fi
if [ -f docs/STATUS.md ]; then
    lines=$(wc -l < docs/STATUS.md)
    echo; echo "=== docs/STATUS.md ==="
    if [ "$lines" -le 100 ]; then
        cat docs/STATUS.md
    else
        # Too long to print whole: always show the header and the Next step section, so the
        # one thing a fresh session needs is never cut off.
        head -n 8 docs/STATUS.md
        echo "..."
        awk '/^## Next step/{f=1} f&&/^## /&&!/^## Next step/{exit} f' docs/STATUS.md
        echo "... (STATUS.md is $lines lines; read the rest, and trim it back under ~80 lines)"
    fi
fi
echo
echo "Follow the session protocol in CLAUDE.md. Resume from 'Next step' if a milestone is in progress."
exit 0

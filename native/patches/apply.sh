#!/usr/bin/env bash
# Apply (or reverse) the Crossover-IOS fork patches onto the pinned Wine
# submodule. The submodule stays at its pinned commit; these patches are the
# fork delta, tracked in the main repo so native/wine never needs committing.
#
#   ./apply.sh          apply all patches in ./series (idempotent-ish; skips
#                       patches that are already applied)
#   ./apply.sh --check  verify each patch applies cleanly, change nothing
#   ./apply.sh --reverse  reverse all patches (restore pristine submodule)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
wine="$here/../wine"
series="$here/series"

[ -d "$wine/server" ] || { echo "error: $wine is not a Wine checkout (init the submodule)"; exit 1; }

mode="apply"
case "${1:-}" in
    --check)   mode="check" ;;
    --reverse) mode="reverse" ;;
    "")        ;;
    *) echo "usage: $0 [--check|--reverse]"; exit 2 ;;
esac

mapfile -t patches < <(grep -vE '^\s*(#|$)' "$series")
[ "$mode" = reverse ] && { min=${#patches[@]}; for ((i=min-1;i>=0;i--)); do rev+=("${patches[i]}"); done; patches=("${rev[@]}"); }

for p in "${patches[@]}"; do
    f="$here/$p"
    case "$mode" in
        check)
            git -C "$wine" apply --check "$f" && echo "ok (applies): $p" \
                || { echo "FAIL (does not apply): $p"; exit 1; } ;;
        reverse)
            git -C "$wine" apply -R "$f" && echo "reversed: $p" \
                || echo "skip (not applied): $p" ;;
        apply)
            if git -C "$wine" apply --reverse --check "$f" 2>/dev/null; then
                echo "skip (already applied): $p"
            else
                git -C "$wine" apply "$f" && echo "applied: $p"
            fi ;;
    esac
done
echo "done ($mode)."

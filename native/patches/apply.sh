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

applied_for_check=()
check_unwind() {
    for ((j=${#applied_for_check[@]}-1; j>=0; j--)); do
        git -C "$wine" apply -R "$here/${applied_for_check[j]}" || true
    done
}

for p in "${patches[@]}"; do
    f="$here/$p"
    case "$mode" in
        check)
            # Patches are sequential diffs, so each must be checked against a
            # tree with its predecessors applied: apply for real as we go and
            # unwind at the end (or on failure), leaving the tree untouched.
            if git -C "$wine" apply --check "$f" && git -C "$wine" apply "$f"; then
                echo "ok (applies): $p"
                applied_for_check+=("$p")
            else
                echo "FAIL (does not apply): $p"
                check_unwind
                exit 1
            fi ;;
        reverse)
            git -C "$wine" apply -R "$f" && echo "reversed: $p" \
                || echo "skip (not applied): $p" ;;
        apply)
            # NB: a failed apply must be loud. Silently continuing leaves the
            # tree half-patched, which looks like "the work vanished".
            if git -C "$wine" apply --reverse --check "$f" 2>/dev/null; then
                echo "skip (already applied): $p"
            elif git -C "$wine" apply "$f"; then
                echo "applied: $p"
            else
                echo "FAIL (does not apply): $p" >&2
                exit 1
            fi ;;
    esac
done
[ "$mode" = check ] && check_unwind >/dev/null
echo "done ($mode)."

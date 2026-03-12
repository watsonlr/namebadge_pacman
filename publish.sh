#!/usr/bin/env bash
# publish.sh — copy tetris_game.bin to the org Pages repo and push
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${SCRIPT_DIR}/build/tetris_game.bin"
PAGES_REPO="${HOME}/Documents/Repositories/byu-i-ebadge.github.io"
DEST="${PAGES_REPO}/apps"

# ── Sanity checks ────────────────────────────────────────────────────────────
if [[ ! -f "${BIN}" ]]; then
    echo "ERROR: binary not found at ${BIN}"
    echo "       Run 'idf.py build' first."
    exit 1
fi

if [[ ! -d "${PAGES_REPO}/.git" ]]; then
    echo "ERROR: Pages repo not found at ${PAGES_REPO}"
    echo "       Run: git clone git@github.com:BYU-I-eBadge/byu-i-ebadge.github.io.git ${PAGES_REPO}"
    exit 1
fi

# ── Copy ─────────────────────────────────────────────────────────────────────
mkdir -p "${DEST}"
cp "${BIN}" "${DEST}/tetris_game.bin"
echo "Copied tetris_game.bin → ${DEST}/"

# ── Commit & push ─────────────────────────────────────────────────────────────
cd "${PAGES_REPO}"

if git diff --quiet --cached && git diff --quiet apps/tetris_game.bin; then
    echo "No changes — binary is identical to what's already committed."
    exit 0
fi

git add apps/tetris_game.bin
git commit -m "Update tetris_game.bin ($(date '+%Y-%m-%d %H:%M'))"
git push

echo ""
echo "Done. Live at: https://byu-i-ebadge.github.io/apps/tetris_game.bin"

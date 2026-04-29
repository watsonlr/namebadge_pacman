#!/usr/bin/env bash
# publish.sh — copy pacman_game.bin to the org Pages repo and push
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${SCRIPT_DIR}/build/pacman_game.bin"
PAGES_REPO="${HOME}/Documents/Repositories/byu-i-ebadge.github.io"
DEST="${PAGES_REPO}/apps"
MANIFEST="${DEST}/manifest.json"

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

# ── Pull latest before making changes ────────────────────────────────────────
git -C "${PAGES_REPO}" pull

# ── Copy binary ──────────────────────────────────────────────────────────────
mkdir -p "${DEST}"
cp "${BIN}" "${DEST}/pacman.bin"
echo "Copied pacman_game.bin → ${DEST}/pacman.bin"

# ── Update manifest size ─────────────────────────────────────────────────────
SIZE=$(stat -c%s "${DEST}/pacman.bin")
python3 -c "
import json, sys
with open('${MANIFEST}') as f:
    m = json.load(f)
for app in m['apps']:
    if app['name'] == 'Pacman':
        app['size'] = ${SIZE}
with open('${MANIFEST}', 'w') as f:
    json.dump(m, f, indent=2)
    f.write('\n')
"
echo "Updated manifest: Pacman size = ${SIZE}"

# ── Commit & push ─────────────────────────────────────────────────────────────
cd "${PAGES_REPO}"

if git diff --quiet apps/pacman.bin apps/manifest.json; then
    echo "No changes — binary is identical to what's already committed."
    exit 0
fi

git add apps/pacman.bin apps/manifest.json
git commit -m "Update pacman.bin ($(date '+%Y-%m-%d %H:%M'))"
git push

echo ""
echo "Done. Live at: https://byu-i-ebadge.github.io/apps/pacman.bin"

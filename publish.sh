#!/usr/bin/env bash
# publish.sh — build pacman, copy all flash binaries to the org Pages repo,
# update manifest.json in the multi-binary format, and push.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_BIN="${SCRIPT_DIR}/build/pacman_game.bin"
BL_BIN="${SCRIPT_DIR}/build/bootloader/bootloader.bin"
PT_BIN="${SCRIPT_DIR}/build/partition_table/partition-table.bin"

PAGES_REPO="${HOME}/Documents/Repositories/byu-i-ebadge.github.io"
DEST="${PAGES_REPO}/apps"
MANIFEST="${DEST}/manifest.json"
GITHUB_PAGES_BASE="https://byu-i-ebadge.github.io/apps"

APP_NAME="Pacman"
APP_DEST_NAME="pacman.bin"
BL_DEST_NAME="pacman_bl.bin"
PT_DEST_NAME="pacman_pt.bin"

# ── Sanity checks ────────────────────────────────────────────────────────────
for f in "$APP_BIN" "$BL_BIN" "$PT_BIN"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: binary not found at ${f}"
        echo "       Run 'idf.py build' first."
        exit 1
    fi
done

if [[ ! -d "${PAGES_REPO}/.git" ]]; then
    echo "ERROR: Pages repo not found at ${PAGES_REPO}"
    echo "       Run: git clone git@github.com:BYU-I-eBadge/byu-i-ebadge.github.io.git ${PAGES_REPO}"
    exit 1
fi

# ── Pull latest before making changes ────────────────────────────────────────
git -C "${PAGES_REPO}" pull

# ── Copy binaries ─────────────────────────────────────────────────────────────
mkdir -p "${DEST}"
cp "${APP_BIN}" "${DEST}/${APP_DEST_NAME}"
cp "${BL_BIN}"  "${DEST}/${BL_DEST_NAME}"
cp "${PT_BIN}"  "${DEST}/${PT_DEST_NAME}"
echo "Copied ${APP_DEST_NAME}  (factory app,        0x10000)"
echo "Copied ${BL_DEST_NAME}   (ESP-IDF bootloader, 0x1000)"
echo "Copied ${PT_DEST_NAME}   (partition table,    0x8000)"

# ── Update manifest ───────────────────────────────────────────────────────────
APP_SIZE=$(stat -c%s "${DEST}/${APP_DEST_NAME}")

python3 - <<EOF
import json

manifest_path = "${MANIFEST}"
try:
    with open(manifest_path) as f:
        m = json.load(f)
except (FileNotFoundError, json.JSONDecodeError):
    m = {"apps": []}

if "apps" not in m:
    m["apps"] = []

new_entry = {
    "name": "${APP_NAME}",
    "binaries": [
        {"url": "${GITHUB_PAGES_BASE}/${BL_DEST_NAME}",  "address": 0x1000},
        {"url": "${GITHUB_PAGES_BASE}/${PT_DEST_NAME}",  "address": 0x8000},
        {"url": "${GITHUB_PAGES_BASE}/${APP_DEST_NAME}", "address": 0x10000},
    ],
    "size": ${APP_SIZE},
}

replaced = False
for i, app in enumerate(m["apps"]):
    if app.get("name") == "${APP_NAME}":
        m["apps"][i] = new_entry
        replaced = True
        break
if not replaced:
    m["apps"].append(new_entry)

with open(manifest_path, "w") as f:
    json.dump(m, f, indent=2)
    f.write("\n")

print(f"  {'Updated' if replaced else 'Added'} ${APP_NAME} in manifest.json")
EOF

# ── Commit & push ─────────────────────────────────────────────────────────────
cd "${PAGES_REPO}"

if git diff --quiet apps/; then
    echo "No changes — binaries are identical to what's already committed."
    exit 0
fi

git add apps/
git commit -m "Update Pacman ($(date '+%Y-%m-%d %H:%M'))

App:        ${APP_DEST_NAME} (${APP_SIZE} bytes, 0x10000)
Bootloader: ${BL_DEST_NAME} (0x1000)
Partitions: ${PT_DEST_NAME} (0x8000)"
git push

echo ""
echo "Done. Live at: ${GITHUB_PAGES_BASE}/${APP_DEST_NAME}"

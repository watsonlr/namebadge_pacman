#!/usr/bin/env bash
# publish.sh — build Pacman, copy binaries to the org Pages repo,
# update manifest.json for both webflash (multi-binary) and OTA (single url).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_BIN="${SCRIPT_DIR}/build/pacman_game.bin"
BL_BIN="${SCRIPT_DIR}/build/bootloader/bootloader.bin"
PT_BIN="${SCRIPT_DIR}/build/partition_table/partition-table.bin"

GITHUB_PAGES_BASE="https://byu-i-ebadge.github.io/apps"

# Locate the Pages repo — set NAMEBADGE_PAGES_REPO to override
if [[ -z "${NAMEBADGE_PAGES_REPO:-}" ]]; then
    SIBLING="$(cd "${SCRIPT_DIR}/.." && pwd)/byu-i-ebadge.github.io"
    if [[ -d "${SIBLING}/.git" ]]; then
        NAMEBADGE_PAGES_REPO="${SIBLING}"
    fi
fi
if [[ -z "${NAMEBADGE_PAGES_REPO:-}" ]] || [[ ! -d "${NAMEBADGE_PAGES_REPO}/.git" ]]; then
    echo "ERROR: Cannot find the byu-i-ebadge.github.io Pages repo."
    echo "       Clone it, then set the env var:"
    echo "       git clone git@github.com:BYU-I-eBadge/byu-i-ebadge.github.io.git"
    echo "       export NAMEBADGE_PAGES_REPO=/path/to/byu-i-ebadge.github.io"
    exit 1
fi
PAGES_REPO="${NAMEBADGE_PAGES_REPO}"
DEST="${PAGES_REPO}/apps"
MANIFEST="${DEST}/manifest.json"

APP_NAME="Pacman"
APP_DEST_NAME="pacman.bin"
BL_DEST_NAME="pacman_bl.bin"
PT_DEST_NAME="pacman_pt.bin"
ICON_DEST_NAME="pacman_icon.bin"

# Locate icon — look in sibling namebadge-apps/icons/
ICON_SRC=""
for candidate in \
    "${SCRIPT_DIR}/../namebadge-apps/icons/pacman_icon.bin" \
    "${SCRIPT_DIR}/pacman_icon.bin"
do
    if [[ -f "$candidate" ]]; then
        ICON_SRC="$candidate"
        break
    fi
done

# ── Build ─────────────────────────────────────────────────────────────────────
echo "Building Pacman..."
source /home/lynn/esp/esp-idf/export.sh 2>/dev/null
idf.py -C "${SCRIPT_DIR}" build

# ── Sanity checks ─────────────────────────────────────────────────────────────
for f in "$APP_BIN" "$BL_BIN" "$PT_BIN"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: binary not found at ${f}"
        exit 1
    fi
done

# ── Pull latest before making changes ─────────────────────────────────────────
git -C "${PAGES_REPO}" pull

# ── Copy binaries ─────────────────────────────────────────────────────────────
mkdir -p "${DEST}"
cp "${APP_BIN}" "${DEST}/${APP_DEST_NAME}"
cp "${BL_BIN}"  "${DEST}/${BL_DEST_NAME}"
cp "${PT_BIN}"  "${DEST}/${PT_DEST_NAME}"
echo "Copied ${APP_DEST_NAME}    (app,               0x10000)"
echo "Copied ${BL_DEST_NAME} (ESP-IDF bootloader, 0x0)"
echo "Copied ${PT_DEST_NAME} (partition table,    0x8000)"

if [[ -n "$ICON_SRC" ]]; then
    cp "${ICON_SRC}" "${DEST}/${ICON_DEST_NAME}"
    echo "Copied ${ICON_DEST_NAME}  (OTA tile icon)"
fi

# ── Compute sha256 and size of app binary ─────────────────────────────────────
APP_SIZE=$(stat -c%s "${DEST}/${APP_DEST_NAME}")
APP_SHA256=$(sha256sum "${DEST}/${APP_DEST_NAME}" | awk '{print $1}')

# ── Update manifest ───────────────────────────────────────────────────────────
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

old_ver = 0
for app in m["apps"]:
    if app.get("name") == "${APP_NAME}":
        old_ver = int(app.get("version", 0))
        break
new_ver = old_ver + 1

icon_url = "${GITHUB_PAGES_BASE}/${ICON_DEST_NAME}" if "${ICON_SRC}" else ""

# Entry satisfies both consumers:
#   webflash site  → reads "binaries" (bl + pt + ota_data + app at their flash addresses)
#   badge OTA menu → reads "url" / "size" / "sha256" (app binary only, streamed to OTA slot)
new_entry = {
    "name": "${APP_NAME}",
    "version": new_ver,
    "url": "${GITHUB_PAGES_BASE}/${APP_DEST_NAME}",
    "size": ${APP_SIZE},
    "sha256": "${APP_SHA256}",
    "binaries": [
        {"url": "${GITHUB_PAGES_BASE}/${BL_DEST_NAME}",  "address": 0x0},
        {"url": "${GITHUB_PAGES_BASE}/${PT_DEST_NAME}",  "address": 0x8000},
        {"url": "${GITHUB_PAGES_BASE}/${APP_DEST_NAME}", "address": 0x10000},
    ],
}
if icon_url:
    new_entry["icon"] = icon_url

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

action = "Updated" if replaced else "Added"
print(f"  {action} ${APP_NAME} v{new_ver} in manifest.json ({${APP_SIZE}} bytes, sha256={str('${APP_SHA256}')[:16]}...)")
EOF

# ── Commit & push ─────────────────────────────────────────────────────────────
cd "${PAGES_REPO}"

if git diff --quiet apps/; then
    echo "No changes — binaries are identical to what's already committed."
    exit 0
fi

git add apps/
git commit -m "Update Pacman ($(date '+%Y-%m-%d %H:%M'))

App:        ${APP_DEST_NAME} (${APP_SIZE} bytes, 0x10000)  sha256=${APP_SHA256}
Bootloader: ${BL_DEST_NAME} (0x0)
Partitions: ${PT_DEST_NAME} (0x8000)"
git push

echo ""
echo "Done. Live at: ${GITHUB_PAGES_BASE}/${APP_DEST_NAME}"
echo "  Webflash: select 'Pacman' in the Single Program Flash dropdown"
echo "  OTA:      badge bootloader will find Pacman in the app catalog"

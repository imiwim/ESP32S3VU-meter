#!/usr/bin/env bash
# all.sh — ESP32VU-meter full dev cycle (one command from edit to running device).
#
# Steps:
#   1. Read PREV build label from build/generated/project_build_info.h.
#   2. Changes.py repair-headers — normalize Changes.txt section headers.
#   3. prompt.sh append --since-build PREV — import new Cursor prompts (+ agent replies).
#   4. Changes.sh — optional manual append when CURSOR_PROMPT env is set.
#   5. rsync entire repo (minus prior snapshot dirs) into ${PREV}/ for reproducible diff.
#   6. BUILD_LABEL_WALL_CLOCK=1 idf.py fullclean reconfigure build — fresh label + stamps.
#   7. latest_mtime.py --resync-stamps — align /* BUILD_NUMBER: … */ on every source file.
#   8. idf.py flash monitor on /dev/ttyACM0 at ESPBAUD=921600.
#
# See ARCHIVE.md.

set -euo pipefail

#~/killtty.sh "/dev/ttyACM0" "KILL"
export ESPBAUD="${ESPBAUD:-921600}"
PORT="${IDF_PORT:-/dev/ttyACM0}"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_INFO="${SCRIPT_DIR}/build/generated/project_build_info.h"

PREV=""
if [ -f "$BUILD_INFO" ]; then
  PREV=$(sed -n 's/^#define PROJECT_BUILD_NUMBER "\(.*\)"/\1/p' "$BUILD_INFO" | head -n1)
fi

if [[ -n "$PREV" ]]; then
  export PROMPT_BUILD_LABEL="$PREV"
fi
if [[ -f "${SCRIPT_DIR}/Changes.py" ]]; then
  python3 "${SCRIPT_DIR}/Changes.py" repair-headers ${PREV:+--build "$PREV"} || true
fi
if [[ -f "${SCRIPT_DIR}/prompt.sh" ]]; then
  if [[ -n "$PREV" ]]; then
    bash "${SCRIPT_DIR}/prompt.sh" append --since-build "$PREV" || true
  else
    bash "${SCRIPT_DIR}/prompt.sh" append || true
  fi
else
  echo "all.sh: ${SCRIPT_DIR}/prompt.sh not found; skipping transcript extract." >&2
fi
if [ -n "${CURSOR_PROMPT:-}" ] && [[ -f "${SCRIPT_DIR}/Changes.sh" ]]; then
  "${SCRIPT_DIR}/Changes.sh" "${CURSOR_PROMPT}"
fi

# Exclude prior YYMMDD.HHMMSS* snapshots and regenerable / bulky trees.
SNAPSHOT_EXCLUDE=(
  '--exclude=/[0-9][0-9][0-9][0-9][0-9][0-9].[0-9][0-9][0-9][0-9][0-9][0-9]*/'
  '--exclude=/build/'
  '--exclude=/managed_components/'
  '--exclude=/.cache/'
  '--exclude=/.pio/'
  '--exclude=/ESP32VU-meter_241230/'
)
if [ -n "$PREV" ]; then
  DEST="${SCRIPT_DIR}/${PREV}"
  rsync -a "${SNAPSHOT_EXCLUDE[@]}" "${SCRIPT_DIR}/" "${DEST}/"
  # Keep the label header even though build/ is excluded from the tree copy.
  if [[ -f "$BUILD_INFO" ]]; then
    mkdir -p "${DEST}/build/generated"
    cp -a "$BUILD_INFO" "${DEST}/build/generated/project_build_info.h"
  fi
fi

export BUILD_LABEL_WALL_CLOCK=1
export VU_BUILD_LABEL="$(python3 -c 'from datetime import datetime; print(datetime.now().strftime("%y%m%d.%H%M%S"))')"
export PROJECT_BUILD_LABEL="$VU_BUILD_LABEL"
echo "all.sh: build label ${VU_BUILD_LABEL}" >&2

idf.py fullclean reconfigure build

if [[ -f "$BUILD_INFO" ]]; then
  python3 "${SCRIPT_DIR}/tools/latest_mtime.py" "${SCRIPT_DIR}" \
    --header "${BUILD_INFO}" --resync-stamps || true
fi

#idf.py -p "$PORT" -b "$ESPBAUD" flash monitor

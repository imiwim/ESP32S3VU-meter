#!/usr/bin/env bash
# ball.sh — Update Changes.txt, then snapshot into ${PROJECT_BUILD_NUMBER}/ (no build/flash).
#
# 1. Changes.py repair-headers + prompt.sh append (Cursor transcripts since this build).
# 2. Optional Changes.sh when CURSOR_PROMPT is set.
# 3. rsync repo → ./YYMMDD.HHMMSS/ (excludes other snapshot dirs).
# See ARCHIVE.md.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_INFO="${SCRIPT_DIR}/build/generated/project_build_info.h"

BUILD=""
if [[ -f "$BUILD_INFO" ]]; then
  BUILD=$(sed -n 's/^#define PROJECT_BUILD_NUMBER "\(.*\)"/\1/p' "$BUILD_INFO" | head -n1)
fi

if [[ -z "$BUILD" ]]; then
  echo "ball.sh: no PROJECT_BUILD_NUMBER in ${BUILD_INFO}" >&2
  echo "ball.sh: run idf.py build (or ./all.sh) first." >&2
  exit 1
fi

export PROMPT_BUILD_LABEL="$BUILD"

echo "ball.sh: updating Changes.txt (build ${BUILD})…" >&2
if [[ -f "${SCRIPT_DIR}/Changes.py" ]]; then
  python3 "${SCRIPT_DIR}/Changes.py" repair-headers --build "$BUILD" || true
fi
if [[ -f "${SCRIPT_DIR}/prompt.sh" ]]; then
  bash "${SCRIPT_DIR}/prompt.sh" append --since-build "$BUILD" || true
else
  echo "ball.sh: ${SCRIPT_DIR}/prompt.sh not found; skipping transcript extract." >&2
fi
if [[ -n "${CURSOR_PROMPT:-}" ]] && [[ -f "${SCRIPT_DIR}/Changes.sh" ]]; then
  "${SCRIPT_DIR}/Changes.sh" "${CURSOR_PROMPT}"
fi

DEST="${SCRIPT_DIR}/${BUILD}"
SNAPSHOT_EXCLUDE=(
  '--exclude=/[0-9][0-9][0-9][0-9][0-9][0-9].[0-9][0-9][0-9][0-9][0-9][0-9]*/'
  '--exclude=/build/'
  '--exclude=/managed_components/'
  '--exclude=/.cache/'
  '--exclude=/.pio/'
  '--exclude=/ESP32VU-meter_241230/'
)

echo "ball.sh: snapshot build ${BUILD} → ${DEST}/" >&2
rsync -a "${SNAPSHOT_EXCLUDE[@]}" "${SCRIPT_DIR}/" "${DEST}/"
if [[ -f "$BUILD_INFO" ]]; then
  mkdir -p "${DEST}/build/generated"
  cp -a "$BUILD_INFO" "${DEST}/build/generated/project_build_info.h"
fi
echo "ball.sh: done." >&2

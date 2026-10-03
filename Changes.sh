#!/usr/bin/env bash
# Changes.sh — append one entry to Changes.txt via Changes.py (build + local time + sha256 + prompt).
# Precedence: argv > CURSOR_PROMPT env > stdin. Used by all.sh when CURSOR_PROMPT is set.
# See ARCHIVE.md.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "${SCRIPT_DIR}/Changes.py" "$@"

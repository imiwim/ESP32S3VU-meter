#!/usr/bin/env bash
# prompt.sh — ESP32VU-meter helper: read Cursor agent transcripts (JSONL), extract user prompts,
# append new entries to Changes.txt (dedupe by sha256). Used by all.sh with append --since-build PREV.
# Transcripts: ~/.cursor/projects/<workspace-slug>/agent-transcripts/**/*.jsonl
# Workspace slug = canonical path of this repo without leading '/', '/' -> '-'.
# See ARCHIVE.md.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Cursor's ~/.cursor/projects/<slug>/ matches the opened workspace path (slashes -> hyphens).
# Use both logical and physical paths: symlinks can make slug differ from the folder Cursor created.
WORKSPACE_LOGICAL=$(cd -- "$SCRIPT_DIR" && pwd)
WORKSPACE_PHYSICAL=$(cd -- "$SCRIPT_DIR" && pwd -P 2>/dev/null || echo "$WORKSPACE_LOGICAL")
OUTPUT_FILE="${OUTPUT_FILE:-Changes.txt}"
CHANGES_TXT="${SCRIPT_DIR}/${OUTPUT_FILE}"
MARKER_FILE="${SCRIPT_DIR}/.changes_last_build"
CURSOR_PROJECTS="${CURSOR_PROJECTS:-$HOME/.cursor/projects}"
# Optional: absolute path to agent-transcripts (skips slug detection).
TRANSCRIPT_ROOT="${TRANSCRIPT_ROOT:-}"

# Truncate and replace OUTPUT_FILE (manual last/default only; all.sh uses append).
write_prompt_txt() {
  printf '%s\n' "${1:-}" >"${SCRIPT_DIR}/${OUTPUT_FILE}"
}

workspace_slug() {
  echo "${1#/}" | tr '/' '-'
}

# Cursor's ~/.cursor/projects/<slug>/ often uses hyphens where the workspace path uses underscores
# (e.g. folder BasicVacIacMonitor → slug …-BasicVacIacMonitor vs Cursor …-ESP32S3-AN9640).
workspace_slug_hyphen_variant() {
  printf '%s' "$1" | tr '_' '-'
}

# Cursor project hash dirs to search (dedupe). Slugs from logical + physical pwd; fuzzy match on basename.
list_cursor_project_roots() {
  local s sh bn bn_alt d
  {
    for _wr in "$WORKSPACE_LOGICAL" "$WORKSPACE_PHYSICAL"; do
      s=$(workspace_slug "$_wr")
      [[ -z "$s" ]] && continue
      printf '%s\n' "$CURSOR_PROJECTS/$s"
      printf '%s\n' "$CURSOR_PROJECTS/${s}0"
      sh=$(workspace_slug_hyphen_variant "$s")
      if [[ "$sh" != "$s" ]]; then
        printf '%s\n' "$CURSOR_PROJECTS/$sh"
        printf '%s\n' "$CURSOR_PROJECTS/${sh}0"
      fi
    done
    bn=$(basename "$WORKSPACE_LOGICAL")
    bn_alt=$(workspace_slug_hyphen_variant "$bn")
    if [[ -d "$CURSOR_PROJECTS" && ${#bn} -ge 4 ]]; then
      shopt -s nullglob
      for d in "$CURSOR_PROJECTS"/*; do
        [[ -d "$d" ]] || continue
        [[ "${d##*/}" == *"$bn"* ]] || [[ "${d##*/}" == *"$bn_alt"* ]] || continue
        printf '%s\n' "$d"
      done
      shopt -u nullglob
    fi
  } | sort -u
}

find_transcript_jsonl() {
  if [[ -n "$TRANSCRIPT_ROOT" ]]; then
    if [[ ! -d "$TRANSCRIPT_ROOT" ]]; then
      echo "TRANSCRIPT_ROOT is not a directory: $TRANSCRIPT_ROOT" >&2
      return 1
    fi
    find "$TRANSCRIPT_ROOT" -name '*.jsonl' -type f 2>/dev/null || true
    return 0
  fi
  if [[ ! -d "$CURSOR_PROJECTS" ]]; then
    echo "CURSOR_PROJECTS not found: $CURSOR_PROJECTS (set HOME or CURSOR_PROJECTS)" >&2
    return 1
  fi

  local combined slug_probe
  # Do not skip roots with [[ -d ]] — some environments mis-report; find is authoritative.
  combined=$(
    while IFS= read -r root; do
      [[ -z "$root" ]] && continue
      find "$root" -name '*.jsonl' -type f 2>/dev/null || true
    done < <(list_cursor_project_roots) | sort -u
  )
  # Last resort: scan ~/.cursor/projects for *.jsonl whose path contains this workspace slug
  # (helps when per-project roots are invisible to find in a sandboxed wrapper).
  if [[ -z "$combined" && -d "$CURSOR_PROJECTS" ]]; then
    slug_probe=$(workspace_slug "$WORKSPACE_LOGICAL")
    slug_alt=$(workspace_slug_hyphen_variant "$slug_probe")
    combined=$(
      find "$CURSOR_PROJECTS" -type f -name '*.jsonl' 2>/dev/null \
        | grep -F -e "$slug_probe" -e "$slug_alt" || true
    )
    [[ -n "$combined" ]] && combined=$(printf '%s\n' "$combined" | sort -u)
  fi
  if [[ -z "$combined" ]]; then
    echo "No Cursor transcript .jsonl found." >&2
    echo "  WORKSPACE_LOGICAL=$WORKSPACE_LOGICAL" >&2
    echo "  WORKSPACE_PHYSICAL=$WORKSPACE_PHYSICAL" >&2
    echo "  CURSOR_PROJECTS=$CURSOR_PROJECTS" >&2
    echo "Try: $0 debug" >&2
    echo "Or:  export TRANSCRIPT_ROOT=\"\$(find \"\$HOME/.cursor/projects\" -type f -name '*.jsonl' -path '*/agent-transcripts/*' 2>/dev/null | head -1 | xargs -r dirname)\"" >&2
    return 1
  fi
  printf '%s\n' "$combined"
}

usage() {
  cat >&2 <<EOF
Usage:
  $(basename "$0") append [--since-build BUILD]   Append new prompts to Changes.txt (default for all.sh)
  $(basename "$0") [-s|--suffix ARG] last [N]     Print last N prompts; overwrites ${OUTPUT_FILE} (manual)
  $(basename "$0")                                 Last user prompt overwrites ${OUTPUT_FILE}
  $(basename "$0") search REGEX                    Search user prompts in all transcript JSONL (Python re, multiline)
  $(basename "$0") paths                           List discovered .jsonl paths
  $(basename "$0") debug                           Show paths, slugs, and project roots (no read)

  $(basename "$0") -h | --help

append:
  Reads all user prompts from Cursor transcripts (chronological, dedupe by sha256).
  Skips prompts already recorded in Changes.txt. Each entry uses
  "=== Run BUILD YYYY-MM-DDTHH:MM:SS±HH:MM ===" (local wall time; build from --since-build,
  build/generated/project_build_info.h, or .changes_last_build). When --since-build BUILD
  differs from .changes_last_build, inserts a "=== Build BUILD ===" section header before
  new entries. Updates .changes_last_build after a successful append.

Example: $(basename "$0") append --since-build 260605.061803

WORKSPACE_LOGICAL=$WORKSPACE_LOGICAL
WORKSPACE_PHYSICAL=$WORKSPACE_PHYSICAL
CURSOR_PROJECTS=$CURSOR_PROJECTS
TRANSCRIPT_ROOT=${TRANSCRIPT_ROOT:-<auto from workspace folder>}
EOF
}

pick_newest_jsonl() {
  local -a files
  mapfile -t files < <(find_transcript_jsonl | sort -u) || true
  if [[ ${#files[@]} -eq 0 || -z "${files[0]:-}" ]]; then
    echo "No .jsonl transcripts (slug logical=$(workspace_slug "$WORKSPACE_LOGICAL") physical=$(workspace_slug "$WORKSPACE_PHYSICAL"))." >&2
    return 1
  fi
  printf '%s\n' "${files[@]}" | xargs -r ls -t 2>/dev/null | head -1
}

py_last_n() {
  local n=$1
  local path=$2
  python3 -c "
import json, sys
n = int(sys.argv[1])
path = sys.argv[2]
buf = []
with open(path, encoding='utf-8', errors='replace') as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            o = json.loads(line)
        except json.JSONDecodeError:
            continue
        if o.get('role') != 'user':
            continue
        parts = []
        for c in o.get('message', {}).get('content', []) or []:
            if isinstance(c, dict) and c.get('type') == 'text':
                parts.append(c.get('text', '') or '')
        text = ''.join(parts)
        text = text.replace('<user_query>', '').replace('</user_query>', '').strip()
        if text:
            buf.append(text)
take = min(max(n, 1), len(buf))
sel = buf[-take:] if take else []
print('\\n---\\n'.join(sel))
" "$n" "$path"
}

py_search() {
  local pattern=$1
  shift
  python3 -c "
import json, re, sys
pattern = sys.argv[1]
paths = sys.argv[2:]
cre = re.compile(pattern, re.MULTILINE | re.DOTALL)
for path in paths:
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    o = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if o.get('role') != 'user':
                    continue
                parts = []
                for c in o.get('message', {}).get('content', []) or []:
                    if isinstance(c, dict) and c.get('type') == 'text':
                        parts.append(c.get('text', '') or '')
                text = ''.join(parts)
                text = text.replace('<user_query>', '').replace('</user_query>', '').strip()
                if text and cre.search(text):
                    print(f'== {path} ==')
                    print(text)
                    print()
    except OSError as e:
        print(f'# skip {path}: {e}', file=sys.stderr)
" "$pattern" "$@"
}

migrate_legacy_changes_txt() {
  if [[ -f "$CHANGES_TXT" ]]; then
    return 0
  fi
  shopt -s nullglob
  local -a legacy=( "${SCRIPT_DIR}"/Changes_*.txt )
  shopt -u nullglob
  if [[ ${#legacy[@]} -eq 0 ]]; then
    return 0
  fi
  local newest
  newest=$(ls -t "${legacy[@]}" 2>/dev/null | head -1)
  if [[ -n "$newest" && -f "$newest" ]]; then
    cp "$newest" "$CHANGES_TXT"
    echo "prompt.sh: seeded ${CHANGES_TXT} from $(basename "$newest")" >&2
  fi
}

py_append_since_build() {
  local build_label=${1:-}
  shift
  local -a py_args=(append-transcripts --changes "$CHANGES_TXT" --marker "$MARKER_FILE")
  if [[ -n "$build_label" ]]; then
    py_args+=(--build "$build_label")
  elif [[ -n "${PROMPT_BUILD_LABEL:-}" ]]; then
    py_args+=(--build "$PROMPT_BUILD_LABEL")
  fi
  python3 "${SCRIPT_DIR}/Changes.py" "${py_args[@]}" "$@"
}

# --- main ---

# Consume leading -s|--suffix ARG → OUTPUT_FILE=Changes_<ARG>.txt (legacy manual use).
declare -a _argv=()
while [[ $# -gt 0 ]]; do
  case $1 in
    -s|--suffix)
      [[ -n "${2:-}" ]] || {
        echo "prompt.sh: $1 requires an argument (output becomes Changes_<arg>.txt)" >&2
        exit 2
      }
      if [[ "$2" =~ ^[a-zA-Z0-9_.-]+$ ]]; then
        OUTPUT_FILE="Changes_$2.txt"
      else
        echo "prompt.sh: suffix must match ^[a-zA-Z0-9_.-]+$ (got: $2)" >&2
        exit 2
      fi
      shift 2
      ;;
    *)
      _argv+=("$1")
      shift
      ;;
  esac
done
set -- "${_argv[@]}"

if [[ $# -eq 1 && ($1 == '-h' || $1 == '--help') ]]; then
  usage
  exit 0
fi

if [[ $# -eq 1 && $1 == help ]]; then
  usage
  exit 0
fi

if [[ $# -eq 0 ]]; then
  newest=$(pick_newest_jsonl) || exit 1
  last_one=$(py_last_n 1 "$newest")
  write_prompt_txt "$last_one"
  #echo "Overwrote ${SCRIPT_DIR}/${OUTPUT_FILE}"
  if [[ -n "$last_one" ]]; then
    echo "--- preview ---"
    echo "$last_one" | head -c 500
    echo
  fi
  exit 0
fi

case $1 in
  append)
    shift
    build_label=""
    while [[ $# -gt 0 ]]; do
      case $1 in
        --since-build)
          [[ -n "${2:-}" ]] || {
            echo "append: --since-build requires BUILD label" >&2
            exit 2
          }
          build_label=$2
          shift 2
          ;;
        *)
          echo "append: unknown option: $1" >&2
          exit 2
          ;;
      esac
    done
    migrate_legacy_changes_txt
    mapfile -t _all < <(find_transcript_jsonl | sort -u)
    if [[ ${#_all[@]} -eq 0 || -z "${_all[0]:-}" ]]; then
      echo "No transcripts found." >&2
      exit 1
    fi
    PROMPT_BUILD_LABEL="$build_label" export PROMPT_BUILD_LABEL
    py_append_since_build "$build_label" "${_all[@]}"
    ;;
  debug)
    echo "WORKSPACE_LOGICAL=$WORKSPACE_LOGICAL" >&2
    echo "WORKSPACE_PHYSICAL=$WORKSPACE_PHYSICAL" >&2
    echo "slug(logical)=$(workspace_slug "$WORKSPACE_LOGICAL")" >&2
    echo "slug(logical,hyphen-variant)=$(workspace_slug_hyphen_variant "$(workspace_slug "$WORKSPACE_LOGICAL")")" >&2
    echo "slug(physical)=$(workspace_slug "$WORKSPACE_PHYSICAL")" >&2
    echo "CURSOR_PROJECTS=$CURSOR_PROJECTS" >&2
    echo "HOME=$HOME" >&2
    echo "--- list_cursor_project_roots ---" >&2
    list_cursor_project_roots >&2
    echo "---" >&2
    exit 0
    ;;
  paths)
    mapfile -t _f < <(find_transcript_jsonl | sort -u)
    if [[ ${#_f[@]} -eq 0 || -z "${_f[0]:-}" ]]; then
      echo "No transcripts found." >&2
      exit 1
    fi
    printf '%s\n' "${_f[@]}"
    ;;
  last)
    n=${2:-3}
    if [[ ! "$n" =~ ^[0-9]+$ ]]; then
      echo "last: N must be a non-negative integer" >&2
      exit 2
    fi
    newest=$(pick_newest_jsonl) || exit 1
    _out=$(py_last_n "$n" "$newest")
    printf '%s\n' "$_out"
    write_prompt_txt "$_out"
    #echo "Overwrote ${SCRIPT_DIR}/${OUTPUT_FILE}" >&2
    ;;
  search)
    if [[ $# -lt 2 ]]; then
      echo "Usage: $0 search REGEX" >&2
      exit 2
    fi
    pattern=$2
    shift 2
    mapfile -t _all < <(find_transcript_jsonl | sort -u)
    if [[ ${#_all[@]} -eq 0 || -z "${_all[0]:-}" ]]; then
      echo "No transcripts found." >&2
      exit 1
    fi
    py_search "$pattern" "${_all[@]}"
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac

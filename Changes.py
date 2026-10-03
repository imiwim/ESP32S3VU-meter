#!/usr/bin/env python3
"""
Changes.py — Cursor prompt/response change log for ESP32VU-meter.

Maintains Changes.txt: each entry has a run header (build label + local ISO time),
sha256 of the prompt body (dedupe key), the user prompt, and an optional --- agent ---
block with the assistant reply extracted from Cursor agent-transcripts JSONL.

Commands:
  append              Add one prompt (and optional agent text) from argv or stdin.
  append-transcripts  Bulk-import new prompts since a build label (used by prompt.sh).
  repair-headers      Normalize === Run/Build === section headers in Changes.txt.

Environment: PROMPT_BUILD_LABEL, CURSOR_PROMPT, CURSOR_RESPONSE, OUTPUT_FILE,
TRANSCRIPT_ROOT, CURSOR_PROJECTS. See ARCHIVE.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

# ISO-8601 timestamp in run headers: UTC Z or local offset (+08:00).
RUN_TS = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:Z|[+-]\d{2}:\d{2})"


def read_build_number(project_root: Path | str) -> str:
    root = Path(project_root)
    header = root / "build" / "generated" / "project_build_info.h"
    if not header.is_file():
        return ""
    text = header.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'#define PROJECT_BUILD_NUMBER "([^"]+)"', text)
    return match.group(1) if match else ""


def read_build_marker(project_root: Path | str) -> str:
    marker = Path(project_root) / ".changes_last_build"
    if not marker.is_file():
        return ""
    return marker.read_text(encoding="utf-8", errors="replace").strip()


def resolve_build_label(project_root: Path | str, explicit: str = "") -> str:
    label = (explicit or os.environ.get("PROMPT_BUILD_LABEL") or "").strip()
    if label:
        return label
    label = read_build_number(project_root).strip()
    if label:
        return label
    label = read_build_marker(project_root).strip()
    if label:
        return label
    return "unknown"


def format_run_header(ts: str, build: str) -> str:
    build = (build or "").strip() or "unknown"
    return f"=== Run {build} {ts} ==="


def local_timestamp_iso(when: datetime | None = None) -> str:
    """Local wall time as ISO-8601 with numeric offset (e.g. 2026-06-22T09:53:54+08:00)."""
    dt = (when or datetime.now()).astimezone()
    return dt.isoformat(timespec="seconds")


def parse_utc_z_timestamp(ts: str) -> datetime | None:
    if not ts.endswith("Z"):
        return None
    try:
        return datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def normalize_timestamp_to_local(ts: str) -> str:
    utc_dt = parse_utc_z_timestamp(ts)
    if utc_dt is not None:
        return local_timestamp_iso(utc_dt)
    return ts


RUN_HEADER_OLD = re.compile(rf"^=== Run ({RUN_TS}) ===$")
RUN_HEADER_NEW = re.compile(rf"^=== Run (\d{{6}}\.\d{{6}}) ({RUN_TS}) ===$")
BUILD_SECTION = re.compile(r"^=== Build (\S+) ===$")


def repair_run_headers(changes_path: Path, default_build: str) -> int:
    """Rewrite legacy `=== Run <iso> ===` lines to include a build label."""
    if not changes_path.is_file():
        return 0

    default_build = (default_build or "unknown").strip() or "unknown"
    lines = changes_path.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)
    current_build = default_build
    fixed = 0
    out: list[str] = []

    for line in lines:
        stripped = line.rstrip("\n\r")
        build_match = BUILD_SECTION.match(stripped)
        if build_match:
            current_build = build_match.group(1)
            out.append(line)
            continue
        if new_match := RUN_HEADER_NEW.match(stripped):
            build, ts = new_match.group(1), new_match.group(2)
            local_ts = normalize_timestamp_to_local(ts)
            if local_ts != ts:
                out.append(f"{format_run_header(local_ts, build)}\n")
                fixed += 1
            else:
                out.append(line)
            continue
        old_match = RUN_HEADER_OLD.match(stripped)
        if old_match:
            ts = normalize_timestamp_to_local(old_match.group(1))
            out.append(f"{format_run_header(ts, current_build)}\n")
            fixed += 1
            continue
        out.append(line)

    if fixed:
        changes_path.write_text("".join(out), encoding="utf-8")
        print(f"Changes.py: repaired {fixed} run header(s) in {changes_path}", file=sys.stderr)
    return fixed


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write_prompt_block(out, prompt: str, build: str, response: str = "") -> None:
    ts = local_timestamp_iso()
    out.write(f"{format_run_header(ts, build)}\n")
    out.write(f"sha256: {sha256_text(prompt)}\n")
    out.write(prompt)
    out.write("\n")
    if response:
        out.write("\n--- agent ---\n")
        out.write(response)
    out.write("\n\n")


def append_entry(changes_path: Path, prompt: str, build: str = "", response: str = "") -> None:
    changes_path.parent.mkdir(parents=True, exist_ok=True)
    with changes_path.open("a", encoding="utf-8") as out:
        write_prompt_block(out, prompt, build, response)


def load_existing_shas(changes_path: Path) -> set[str]:
    shas: set[str] = set()
    if not changes_path.is_file():
        return shas
    with changes_path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if line.startswith("sha256:"):
                shas.add(line.split(":", 1)[1].strip())
    return shas


def extract_role_text(obj: dict, role: str) -> str:
    if obj.get("role") != role:
        return ""
    parts: list[str] = []
    for chunk in obj.get("message", {}).get("content", []) or []:
        if isinstance(chunk, dict) and chunk.get("type") == "text":
            parts.append(chunk.get("text", "") or "")
    text = "".join(parts)
    if role == "user":
        text = text.replace("<user_query>", "").replace("</user_query>", "")
    return text.strip()


def extract_prompt_response_pairs(path: str) -> list[tuple[str, str]]:
    """Return (user prompt, following agent text) pairs in file order."""
    pairs: list[tuple[str, str]] = []
    current_prompt: str | None = None
    response_parts: list[str] = []

    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            role = obj.get("role")
            if role == "user":
                prompt = extract_role_text(obj, "user")
                if current_prompt is not None:
                    response = "\n\n".join(response_parts).strip()
                    pairs.append((current_prompt, response))
                if prompt:
                    current_prompt = prompt
                    response_parts = []
                continue

            if role == "assistant" and current_prompt is not None:
                text = extract_role_text(obj, "assistant")
                if text:
                    response_parts.append(text)

    if current_prompt is not None:
        response = "\n\n".join(response_parts).strip()
        pairs.append((current_prompt, response))

    return pairs


def extract_user_prompts(path: str) -> list[str]:
    return [prompt for prompt, _ in extract_prompt_response_pairs(path) if prompt]


def collect_prompts_chronological(paths: list[str]) -> list[tuple[str, str]]:
    ordered = sorted((path for path in paths if os.path.isfile(path)), key=os.path.getmtime)
    seen_sha: set[str] = set()
    result: list[tuple[str, str]] = []
    for path in ordered:
        for prompt, response in extract_prompt_response_pairs(path):
            digest = sha256_text(prompt)
            if digest in seen_sha:
                continue
            seen_sha.add(digest)
            result.append((prompt, response))
    return result


def append_transcripts(
    project_root: Path,
    changes_path: Path,
    marker_path: Path,
    jsonl_paths: list[str],
    build_label: str = "",
) -> int:
    run_build = resolve_build_label(project_root, build_label)
    repair_run_headers(changes_path, run_build)
    existing = load_existing_shas(changes_path)
    old_build = marker_path.read_text(encoding="utf-8", errors="replace").strip() if marker_path.is_file() else ""
    pairs = collect_prompts_chronological(jsonl_paths)
    new_pairs = [(prompt, response) for prompt, response in pairs if sha256_text(prompt) not in existing]

    if not new_pairs:
        if build_label and build_label != old_build:
            marker_path.write_text(build_label, encoding="utf-8")
        print("Changes.py: no new prompts to append", file=sys.stderr)
        return 0

    changes_path.parent.mkdir(parents=True, exist_ok=True)
    with changes_path.open("a", encoding="utf-8") as out:
        if build_label and build_label != old_build:
            out.write(f"=== Build {build_label} ===\n\n")
        for prompt, response in new_pairs:
            write_prompt_block(out, prompt, run_build, response)

    if build_label:
        marker_path.write_text(build_label, encoding="utf-8")

    print(f"Changes.py: appended {len(new_pairs)} prompt(s) to {changes_path}", file=sys.stderr)
    return len(new_pairs)


def resolve_prompt(argv: list[str]) -> str:
    if argv:
        return " ".join(argv)
    if os.environ.get("CURSOR_PROMPT"):
        return os.environ["CURSOR_PROMPT"]
    if not sys.stdin.isatty():
        return sys.stdin.read()
    return ""


def cmd_append(args: argparse.Namespace) -> int:
    prompt = resolve_prompt(args.prompt).strip()
    if not prompt:
        print("Changes.py: empty prompt", file=sys.stderr)
        return 1

    response = (args.response or os.environ.get("CURSOR_RESPONSE") or "").strip()

    project_root = args.project_root.resolve()
    changes_path = (args.changes or (project_root / "Changes.txt")).resolve()
    build = resolve_build_label(project_root, args.build or "")

    repair_run_headers(changes_path, build)
    append_entry(changes_path, prompt, build, response)
    print(f"Appended to {changes_path}")
    return 0


def cmd_repair_headers(args: argparse.Namespace) -> int:
    project_root = args.project_root.resolve()
    changes_path = (args.changes or (project_root / "Changes.txt")).resolve()
    build = resolve_build_label(project_root, args.build or "")
    repair_run_headers(changes_path, build)
    return 0


def cmd_append_transcripts(args: argparse.Namespace) -> int:
    if not args.jsonl:
        print("Changes.py append-transcripts: no .jsonl paths given", file=sys.stderr)
        return 1

    project_root = args.project_root.resolve()
    changes_path = (args.changes or (project_root / "Changes.txt")).resolve()
    marker_path = (args.marker or (project_root / ".changes_last_build")).resolve()
    append_transcripts(project_root, changes_path, marker_path, args.jsonl, args.build or "")
    return 0


def build_parser() -> argparse.ArgumentParser:
    project_default = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Append prompts and agent responses to Changes.txt")
    parser.add_argument(
        "--project-root",
        type=Path,
        default=project_default,
        help="Repository root (default: directory containing Changes.py)",
    )
    sub = parser.add_subparsers(dest="command")

    append = sub.add_parser("append", help="Append one prompt (default command)")
    append.add_argument("--build", help="Build label (default: PROMPT_BUILD_LABEL, project_build_info.h, marker)")
    append.add_argument("--changes", type=Path, help="Changes.txt path")
    append.add_argument("--response", help="Agent response text (else CURSOR_RESPONSE env)")
    append.add_argument("prompt", nargs="*", help="Prompt text (else CURSOR_PROMPT or stdin)")
    append.set_defaults(func=cmd_append)

    transcripts = sub.add_parser("append-transcripts", help="Append new user prompts + agent replies from Cursor JSONL")
    transcripts.add_argument("--build", help="Build label for --since-build sync")
    transcripts.add_argument("--changes", type=Path, help="Changes.txt path")
    transcripts.add_argument("--marker", type=Path, help=".changes_last_build path")
    transcripts.add_argument("jsonl", nargs="+", help="Transcript .jsonl file paths")
    transcripts.set_defaults(func=cmd_append_transcripts)

    repair = sub.add_parser("repair-headers", help="Backfill build labels on legacy === Run <iso> === lines")
    repair.add_argument("--build", help="Default build label when no === Build … === section applies")
    repair.add_argument("--changes", type=Path, help="Changes.txt path")
    repair.set_defaults(func=cmd_repair_headers)

    return parser


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()

    # Default command: single append (Changes.sh compatibility).
    if not argv or argv[0] not in {"append", "append-transcripts", "repair-headers", "-h", "--help"}:
        args = parser.parse_args(["append", *argv])
    else:
        args = parser.parse_args(argv)

    if args.command == "append":
        return cmd_append(args)
    if args.command == "append-transcripts":
        return cmd_append_transcripts(args)
    if args.command == "repair-headers":
        return cmd_repair_headers(args)

    parser.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

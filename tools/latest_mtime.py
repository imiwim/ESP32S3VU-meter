#!/usr/bin/env python3
"""
@file latest_mtime.py
@brief Emit `PROJECT_BUILD_NUMBER` from the newest file mtime under `main/` and `components/`,
       stamp build comments on `.c`/`.cpp`/`.h`/`.hpp` and a visible build label on project `.md` files,
       write `project_build_info.h`.

Used by CMake in `main/CMakeLists.txt`. File mtimes are preserved when only the build
comment changes so the label still reflects real edits, not stamp churn.
"""

import argparse
import os
import re
import sys
from datetime import datetime

EXCLUDE_DIRS = {
    ".git",
    "build",
    "managed_components",
    "__pycache__",
    ".idea",
    ".vscode",
    ".cursor",
    "node_modules",
    "html",
}

# Firmware source trees scanned for mtime and build-number comments.
WATCH_DIRS = ("main", "components")

# First-party Markdown trees (plus root *.md) stamped with BUILD_NUMBER; not used for mtime label.
MD_WATCH_DIRS = ("Documents", "docs", "tools", "components")

CODE_EXTENSIONS = (".c", ".cpp", ".h", ".hpp")
MD_EXTENSION = ".md"

SNAPSHOT_DIR_RE = re.compile(r"^\d{6}\.\d{6}$")

BUILD_COMMENT_LINE_RE = re.compile(
    r"^(?:/\* BUILD_NUMBER: [0-9]{6}\.[0-9]{6} \*/|// PROJECT_BUILD_NUMBER [0-9]{6}\.[0-9]{6})\n?",
)

BUILD_MD_LINE_RE = re.compile(
    r"^(?:<!-- BUILD_NUMBER: [0-9]{6}\.[0-9]{6} -->|\*\*Build label:\*\* `[0-9]{6}\.[0-9]{6}`)\n\n?",
)


def should_skip_dir(name: str) -> bool:
    if name in EXCLUDE_DIRS:
        return True
    if SNAPSHOT_DIR_RE.match(name):
        return True
    return False


def newest_timestamp(project_root: str) -> float:
    latest = 0.0
    for sub in WATCH_DIRS:
        base = os.path.join(project_root, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if not should_skip_dir(d)]
            for name in filenames:
                if not name.endswith(CODE_EXTENSIONS):
                    continue
                full_path = os.path.join(dirpath, name)
                try:
                    mtime = os.path.getmtime(full_path)
                except OSError:
                    continue
                if mtime > latest:
                    latest = mtime
    return latest


def build_comment_line(build_number: str) -> str:
    return f"/* BUILD_NUMBER: {build_number} */\n"


def build_md_line(build_number: str) -> str:
    return f"**Build label:** `{build_number}`\n\n"


def apply_build_comment(content: str, build_number: str) -> str:
    line = build_comment_line(build_number)
    if BUILD_COMMENT_LINE_RE.match(content):
        return BUILD_COMMENT_LINE_RE.sub(line, content, count=1)
    return line + content


def apply_md_build_comment(content: str, build_number: str) -> str:
    line = build_md_line(build_number)
    if BUILD_MD_LINE_RE.match(content):
        return BUILD_MD_LINE_RE.sub(line, content, count=1)
    return line + content


def iter_source_files(project_root: str):
    for sub in WATCH_DIRS:
        base = os.path.join(project_root, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if not should_skip_dir(d)]
            for name in filenames:
                if name.endswith(CODE_EXTENSIONS):
                    yield os.path.join(dirpath, name)


def stamp_source_files(project_root: str, build_number: str) -> int:
    """Insert or update the build comment at the top of each source file; preserve mtime."""
    updated = 0
    for full_path in iter_source_files(project_root):
        try:
            mtime = os.path.getmtime(full_path)
            with open(full_path, encoding="utf-8", errors="replace") as handle:
                content = handle.read()
        except OSError as exc:
            print(f"[latest_mtime] skip {full_path}: {exc}", file=sys.stderr)
            continue

        new_content = apply_build_comment(content, build_number)
        if new_content == content:
            continue

        try:
            with open(full_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(new_content)
            os.utime(full_path, (mtime, mtime))
            updated += 1
        except OSError as exc:
            print(f"[latest_mtime] failed to stamp {full_path}: {exc}", file=sys.stderr)
    return updated


def iter_markdown_files(project_root: str):
    try:
        for name in os.listdir(project_root):
            if name.endswith(MD_EXTENSION) and os.path.isfile(os.path.join(project_root, name)):
                yield os.path.join(project_root, name)
    except OSError:
        pass

    for sub in MD_WATCH_DIRS:
        base = os.path.join(project_root, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if not should_skip_dir(d)]
            for name in filenames:
                if name.endswith(MD_EXTENSION):
                    yield os.path.join(dirpath, name)


def stamp_file(full_path: str, build_number: str, apply_fn) -> bool:
    """Update build comment; preserve mtime. Returns True if file content changed."""
    try:
        mtime = os.path.getmtime(full_path)
        with open(full_path, encoding="utf-8", errors="replace") as handle:
            content = handle.read()
    except OSError as exc:
        print(f"[latest_mtime] skip {full_path}: {exc}", file=sys.stderr)
        return False

    new_content = apply_fn(content, build_number)
    if new_content == content:
        return False

    try:
        with open(full_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(new_content)
        os.utime(full_path, (mtime, mtime))
        return True
    except OSError as exc:
        print(f"[latest_mtime] failed to stamp {full_path}: {exc}", file=sys.stderr)
        return False


def stamp_markdown_files(project_root: str, build_number: str) -> int:
    """Insert or update visible build label at the top of each project .md file."""
    updated = 0
    for full_path in iter_markdown_files(project_root):
        if stamp_file(full_path, build_number, apply_md_build_comment):
            updated += 1
    return updated


def read_label_from_header(path: str) -> str:
    if not path or not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    match = re.search(r'#define PROJECT_BUILD_NUMBER "([^"]+)"', text)
    return match.group(1) if match else ""


def format_build_label(when: datetime | None = None) -> str:
    return (when or datetime.now()).strftime("%y%m%d.%H%M%S")


def resolve_build_label(args: argparse.Namespace) -> str:
    if args.resync_stamps:
        if not args.header:
            raise SystemExit("--resync-stamps requires --header")
        label = read_label_from_header(args.header)
        if not label:
            raise SystemExit(f"--resync-stamps: no PROJECT_BUILD_NUMBER in {args.header}")
        return label
    if args.build:
        return args.build.strip()
    for env_key in ("VU_BUILD_LABEL", "KITCHEN_BUILD_LABEL", "PROJECT_BUILD_LABEL", "VACIAC_BUILD_LABEL"):
        env_label = os.environ.get(env_key, "").strip()
        if env_label:
            return env_label
    if args.wall_clock:
        return format_build_label()
    timestamp = newest_timestamp(args.root)
    if timestamp <= 0:
        return "000000.000000"
    return datetime.fromtimestamp(timestamp).strftime("%y%m%d.%H%M%S")


def write_header(path: str, build_number: str) -> None:
    if not path:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as header:
        header.write("#pragma once\n")
        header.write(f'#define PROJECT_BUILD_NUMBER "{build_number}"\n')


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build label from newest source mtime; stamp BUILD_NUMBER comments; optional header."
    )
    parser.add_argument("root", help="Project root directory (contains main/ and components/)")
    parser.add_argument("--header", help="Optional header file to generate with build number")
    parser.add_argument(
        "--build",
        help="Use this build label instead of scanning source mtimes",
    )
    parser.add_argument(
        "--wall-clock",
        action="store_true",
        help="Use current local time as build label (used by all.sh)",
    )
    parser.add_argument(
        "--resync-stamps",
        action="store_true",
        help="Stamp sources/markdown from PROJECT_BUILD_NUMBER in --header (no relabel)",
    )
    parser.add_argument(
        "--no-stamp",
        action="store_true",
        help="Skip writing BUILD_NUMBER comments into source files",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    formatted = resolve_build_label(args)

    ansi_blue_bold = "\033[1;34m"
    ansi_reset = "\033[0m"
    # One clean label on stdout for CMake OUTPUT_VARIABLE / project(VERSION …).
    print(formatted, flush=True)

    if not args.no_stamp:
        stamped_src = stamp_source_files(args.root, formatted)
        stamped_md = stamp_markdown_files(args.root, formatted)
        mode = "resync" if args.resync_stamps else "stamp"
        print(
            f"{ansi_blue_bold}[latest_mtime] {mode}: build number {formatted}; "
            f"updated {stamped_src} source + {stamped_md} markdown file(s)"
            f"{ansi_reset}",
            file=sys.stderr,
        )
    else:
        print(
            f"{ansi_blue_bold}[latest_mtime] build number: {formatted}{ansi_reset}",
            file=sys.stderr,
        )

    if args.header and not args.resync_stamps:
        write_header(args.header, formatted)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

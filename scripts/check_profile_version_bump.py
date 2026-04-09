#!/usr/bin/env python3
"""
Verify that all profile versions in the working tree have been incremented
compared to the base branch (default: origin/main).

Usage:
    python3 check_profile_version_bump.py [--base origin/main]

Exit codes:
    0  All versions are strictly greater than the base.
    1  At least one version was not bumped, or an error occurred.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path


def parse_version(version_str: str) -> tuple[int, ...]:
    parts = version_str.strip().split(".")
    return tuple(int(p) for p in parts)


def get_base_version(base_ref: str, rel_path: str) -> str | None:
    """Read a file's content from the given git ref."""
    try:
        content = subprocess.check_output(
            ["git", "show", f"{base_ref}:{rel_path}"],
            stderr=subprocess.DEVNULL,
        ).decode("utf-8")
        data = json.loads(content)
        return data.get("version")
    except (subprocess.CalledProcessError, json.JSONDecodeError, KeyError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Check profile version bump vs base branch")
    parser.add_argument("--base", default="origin/main", help="Base git ref to compare against")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent
    profiles_dir = repo_root / "resources" / "profiles"

    if not profiles_dir.is_dir():
        print(f"ERROR: profiles directory not found: {profiles_dir}", file=sys.stderr)
        return 1

    json_files = sorted(profiles_dir.rglob("*.json"))
    errors = 0
    checked = 0

    for path in json_files:
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            continue

        if "version" not in data:
            continue

        current_str = data["version"]
        rel_path = str(path.relative_to(repo_root))

        base_str = get_base_version(args.base, rel_path)
        if base_str is None:
            # New file, no base version to compare — OK
            checked += 1
            continue

        try:
            current = parse_version(current_str)
            base = parse_version(base_str)
        except ValueError as e:
            print(f"ERROR: unparseable version in {rel_path}: {e}", file=sys.stderr)
            errors += 1
            continue

        checked += 1
        if current <= base:
            print(
                f"ERROR: {rel_path}: version {current_str} is not greater "
                f"than base {base_str}"
            )
            errors += 1

    if checked == 0:
        print("WARNING: no profile JSON files with 'version' key found")
        return 0

    if errors > 0:
        print(f"\nFAILED: {errors} file(s) have a version that was not bumped.")
        return 1

    print(f"OK: all {checked} profile version(s) are greater than {args.base}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

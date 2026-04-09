#!/usr/bin/env python3
"""
Bump the "version" field in all profile JSON files under resources/profiles/.

Usage:
    python3 bump_profile_version.py <new_version>

The new version must be strictly greater than every version found in the
profile files (compared component-wise as integers, e.g. 2.3.3.4 > 2.3.3.3).
"""

import sys
import json
import re
from pathlib import Path


def parse_version(version_str: str) -> tuple[int, ...]:
    """Parse a dotted version string into a tuple of ints."""
    parts = version_str.strip().split(".")
    try:
        return tuple(int(p) for p in parts)
    except ValueError:
        raise ValueError(f"Invalid version format: '{version_str}'")


def find_profile_jsons(profiles_dir: Path) -> list[Path]:
    """Return every .json file under profiles_dir (recursively)."""
    return sorted(profiles_dir.rglob("*.json"))


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <new_version>", file=sys.stderr)
        return 1

    new_version_str = sys.argv[1].strip()
    try:
        new_version = parse_version(new_version_str)
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    script_dir = Path(__file__).resolve().parent
    profiles_dir = script_dir.parent / "resources" / "profiles"

    if not profiles_dir.is_dir():
        print(f"ERROR: profiles directory not found: {profiles_dir}", file=sys.stderr)
        return 1

    json_files = find_profile_jsons(profiles_dir)
    if not json_files:
        print("ERROR: no JSON files found under profiles directory", file=sys.stderr)
        return 1

    # --- First pass: validate that new_version > every current version --------
    version_pattern = re.compile(r'("version"\s*:\s*)"([^"]+)"')
    files_to_update: list[tuple[Path, str]] = []   # (path, current_version_str)
    max_current: tuple[int, ...] = (0,)

    for path in json_files:
        text = path.read_text(encoding="utf-8")
        match = version_pattern.search(text)
        if match is None:
            continue
        cur_str = match.group(2)
        try:
            cur = parse_version(cur_str)
        except ValueError:
            print(f"WARNING: skipping {path.relative_to(profiles_dir)} "
                  f"(unparseable version '{cur_str}')")
            continue
        files_to_update.append((path, cur_str))
        if cur > max_current:
            max_current = cur

    if not files_to_update:
        print("ERROR: no JSON files with a 'version' key found", file=sys.stderr)
        return 1

    if new_version <= max_current:
        max_current_str = ".".join(str(p) for p in max_current)
        print(f"ERROR: new version {new_version_str} must be greater than "
              f"current maximum {max_current_str}", file=sys.stderr)
        return 1

    # --- Second pass: apply the update ----------------------------------------
    updated = 0
    for path, cur_str in files_to_update:
        text = path.read_text(encoding="utf-8")
        new_text = text.replace(f'"version": "{cur_str}"', f'"version": "{new_version_str}"')
        # Handle possible variations in whitespace (tabs, extra spaces)
        if new_text == text:
            new_text = version_pattern.sub(
                lambda m: f'{m.group(1)}"{new_version_str}"', text
            )
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            updated += 1
            print(f"  Updated {path.relative_to(profiles_dir)}: {cur_str} -> {new_version_str}")

    print(f"\nDone: {updated} file(s) updated to version {new_version_str}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

IMAGE_DIR = Path("/Users/giacomoguaresi/Source/GingerRepos/GingerSlicer/resources/images")
CODE_DIR = Path("/Users/giacomoguaresi/Source/GingerRepos/GingerSlicer")
IMAGE_EXTENSIONS = {".png", ".svg", ".ico", ".icns"}
MAX_WORKERS = 12


def is_referenced(pattern: str) -> bool:
    cmd = [
        "git",
        "-C",
        str(CODE_DIR),
        "grep",
        "-F",
        "-q",
        "-e",
        pattern,
        "--",
        ".",
        ":!resources/images/**",
        ":!.git/**",
    ]
    result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if result.returncode == 0:
        return True
    if result.returncode == 1:
        return False
    return False


def process_image(img: Path):
    filename = img.name
    stem = img.stem

    used = is_referenced(filename) or is_referenced(stem)
    if used:
        return "used", filename

    try:
        img.unlink()
        return "deleted", filename
    except Exception as exc:
        return "error", f"{filename}: {exc}"


def main():
    images = [f for f in IMAGE_DIR.iterdir() if f.suffix in IMAGE_EXTENSIONS and f.name != ".DS_Store"]
    total = len(images)

    print(f"Checking {total} images using {MAX_WORKERS} threads...")

    used_count = 0
    deleted_count = 0
    error_count = 0

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
        future_map = {executor.submit(process_image, img): img for img in images}

        for idx, future in enumerate(as_completed(future_map), start=1):
            status, message = future.result()

            if status == "used":
                used_count += 1
                print(f"USED: {message}")
            elif status == "deleted":
                deleted_count += 1
                print(f"DELETED: {message}")
            else:
                error_count += 1
                print(f"ERROR: {message}")

            progress = (idx / total) * 100 if total else 100.0
            print(f"Progress: {idx}/{total} ({progress:.1f}%)")

    print("\n" + "=" * 60)
    print("Summary:")
    print(f"Used: {used_count}")
    print(f"Deleted (unused): {deleted_count}")
    print(f"Errors: {error_count}")
    print("=" * 60)


if __name__ == "__main__":
    main()

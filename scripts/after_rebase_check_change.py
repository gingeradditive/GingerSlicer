import os
import subprocess
import fnmatch
import argparse

# Directory base del progetto
BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# File dove salvare l'elenco dei file modificati
OUTPUT_FILE = os.path.join(BASE_DIR, "changed_files.txt")

# Pattern da escludere (aggiungi liberamente)
EXCLUDE_PATTERNS = [
    "resources/profiles/*",
    "resources/images/*",
    "doc/*",
    "localization/*",
]


def get_changed_files():
    """Restituisce la lista dei file modificati rispetto a upstream/main, esclusi i pattern."""
    print("📋 Recupero elenco file modificati rispetto a upstream/main...")
    try:
        result = subprocess.run(
            ["git", "diff", "upstream/main", "--name-only"],
            cwd=BASE_DIR,
            capture_output=True,
            text=True,
            check=True
        )
        changed = result.stdout.strip().splitlines()
    except subprocess.CalledProcessError as e:
        print("⚠️ Errore eseguendo git diff:", e)
        return []

    # Filtra i file esclusi
    filtered = []
    for fname in changed:
        if not any(fnmatch.fnmatch(fname, pattern) for pattern in EXCLUDE_PATTERNS):
            filtered.append(fname)

    return filtered


def write_changed_files(changed_files):
    """Scrive i file modificati su changed_files.txt."""
    try:
        with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
            for fname in changed_files:
                f.write(fname + "\n")
        print(f"✅ Elenco scritto in {OUTPUT_FILE} ({len(changed_files)} file)")
    except Exception as e:
        print("⚠️ Impossibile scrivere il file changed_files.txt:", e)


def restore_files_from_upstream():
    """Ripristina i file elencati in changed_files.txt alla versione di upstream/main (senza commit)."""
    if not os.path.exists(OUTPUT_FILE):
        print("⚠️ Il file changed_files.txt non esiste. Esegui prima il diff.")
        return

    with open(OUTPUT_FILE, "r", encoding="utf-8") as f:
        files_to_restore = [line.strip() for line in f if line.strip()]

    if not files_to_restore:
        print("ℹ️ Nessun file da ripristinare.")
        return

    print(f"🔄 Ripristino {len(files_to_restore)} file da upstream/main...")
    try:
        subprocess.run(
            ["git", "checkout", "upstream/main", "--"] + files_to_restore,
            cwd=BASE_DIR,
            check=True
        )
        print("✅ Ripristino completato (nessun commit effettuato).")
    except subprocess.CalledProcessError as e:
        print("⚠️ Errore durante il ripristino dei file:", e)


def main():
    parser = argparse.ArgumentParser(
        description="Crea un elenco di file modificati rispetto a upstream/main e consente di ripristinarli."
    )
    parser.add_argument(
        "--restore",
        action="store_true",
        help="Ripristina i file elencati in changed_files.txt alla versione di upstream/main",
    )
    args = parser.parse_args()

    if args.restore:
        restore_files_from_upstream()
    else:
        changed_files = get_changed_files()
        write_changed_files(changed_files)


if __name__ == "__main__":
    main()

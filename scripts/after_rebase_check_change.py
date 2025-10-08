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


def run_git_command(args):
    """Esegue un comando git e ritorna l'output come lista di righe."""
    result = subprocess.run(
        ["git"] + args,
        cwd=BASE_DIR,
        capture_output=True,
        text=True
    )
    if result.returncode != 0:
        return []
    return result.stdout.strip().splitlines()


def get_changed_files():
    print("📋 Recupero elenco file modificati rispetto a upstream/main...")
    try:
        changed = run_git_command(["diff", "upstream/main", "--name-only"])
    except Exception as e:
        print("⚠️ Errore eseguendo git diff:", e)
        return []

    # Filtra i file esclusi
    filtered = []
    for fname in changed:
        if not any(fnmatch.fnmatch(fname, pattern) for pattern in EXCLUDE_PATTERNS):
            filtered.append(fname)

    return filtered


def write_changed_files(changed_files):
    try:
        with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
            for fname in changed_files:
                f.write(fname + "\n")
        print(f"✅ Elenco scritto in {OUTPUT_FILE} ({len(changed_files)} file)")
    except Exception as e:
        print("⚠️ Impossibile scrivere il file changed_files.txt:", e)


def restore_files_from_upstream():
    if not os.path.exists(OUTPUT_FILE):
        print("⚠️ Il file changed_files.txt non esiste. Esegui prima il diff.")
        return

    with open(OUTPUT_FILE, "r", encoding="utf-8") as f:
        files_to_restore = [line.strip() for line in f if line.strip()]

    if not files_to_restore:
        print("ℹ️ Nessun file da ripristinare.")
        return

    print(f"🔍 Verifica esistenza file in upstream/main...")

    # Ottiene l'elenco dei file esistenti in upstream/main
    existing_files = set(run_git_command(["ls-tree", "-r", "--name-only", "upstream/main"]))

    # Filtra solo quelli effettivamente presenti
    valid_files = [f for f in files_to_restore if f in existing_files]
    missing_files = [f for f in files_to_restore if f not in existing_files]

    print(f"🔄 Ripristino {len(valid_files)} file da upstream/main...")
    if missing_files:
        print(f"⚠️ {len(missing_files)} file non esistono in upstream/main e saranno ignorati.")
        for f in missing_files[:10]:  # Mostra solo i primi 10 per non inondare la console
            print(f"   └─ {f}")

    if not valid_files:
        print("ℹ️ Nessun file valido da ripristinare.")
        return

    try:
        subprocess.run(
            ["git", "checkout", "upstream/main", "--"] + valid_files,
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

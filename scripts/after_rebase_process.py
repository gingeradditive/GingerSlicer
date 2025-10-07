import os
import re
import shutil
import subprocess
import fnmatch

BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def diff_changed_files():
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
        changed = []

    output_path = os.path.join(BASE_DIR, "changed_files.txt")
    try:
        with open(output_path, "w", encoding="utf-8") as f:
            for fname in changed:
                f.write(fname + "\n")
        print(f"✅ Elenco scritto in {output_path} ({len(changed)} file)")
    except Exception as e:
        print("⚠️ Impossibile scrivere il file changed_files.txt:", e)

    return changed

def main():
    # --- 0) Lista file modificati ---
    diff_changed_files()

    # --- 1) Pulizia resources/profiles ---
    PROFILES_DIR = os.path.join(BASE_DIR, "resources", "profiles")
    KEEP_PATHS = {
        os.path.join(PROFILES_DIR, "Ginger Additive"),
        os.path.join(PROFILES_DIR, "check_unused_setting_id.py"),
        os.path.join(PROFILES_DIR, "Ginger Additive.json"),
        os.path.join(PROFILES_DIR, "hotend.stl"),
    }

    print("🧹 Pulizia cartella resources/profiles...")
    for item in os.listdir(PROFILES_DIR):
        item_path = os.path.join(PROFILES_DIR, item)
        if item_path not in KEEP_PATHS:
            try:
                if os.path.isdir(item_path):
                    shutil.rmtree(item_path)
                    print(f"  Rimosso folder: {item_path}")
                else:
                    os.remove(item_path)
                    print(f"  Rimosso file: {item_path}")
            except Exception as e:
                print(f"  ⚠️ Errore rimuovendo {item_path}: {e}")

    # --- 2) Rimuovo cartella doc ---
    DOC_DIR = os.path.join(BASE_DIR, "doc")
    if os.path.exists(DOC_DIR):
        print("🗑️ Rimuovo la cartella doc...")
        shutil.rmtree(DOC_DIR, ignore_errors=True)

    # --- 3) Eseguo ConvertSVGtoGingerColor.py ---
    CONVERT_SCRIPT = os.path.join(BASE_DIR, "scripts", "ConvertSVGtoGingerColor.py")
    if os.path.exists(CONVERT_SCRIPT):
        print("🎨 Eseguo ConvertSVGtoGingerColor.py...")
        subprocess.run(["python", CONVERT_SCRIPT], cwd=BASE_DIR, check=False)
    else:
        print("⚠️ Script ConvertSVGtoGingerColor.py non trovato!")

    # --- 4) Ricerca parola "orca" ---
    # print("🔍 Ricerca di 'orca' nel repository...")
    # matches = []
    # for root, dirs, files in os.walk(BASE_DIR):
    #     dirs[:] = [d for d in dirs if d not in [".git", "__pycache__"]]
    #     for file in files:
    #         if file.endswith((".py", ".txt", ".json")):
    #             file_path = os.path.join(root, file)
    #             try:
    #                 with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
    #                     for i, line in enumerate(f, 1):
    #                         if "orca" in line.lower():
    #                             matches.append(f"{file_path}:{i}: {line.strip()}")
    #             except Exception as e:
    #                 print(f"  ⚠️ Impossibile leggere {file_path}: {e}")

    # if matches:
    #     print("\n🚨 Occorrenze trovate della parola 'orca':")
    #     for m in matches:
    #         print(m)
    # else:
    #     print("\n✅ Nessuna occorrenza di 'orca' trovata.")

    # --- 5) Aggiorna versione in version.inc ---
    VERSION_FILE = os.path.join(BASE_DIR, "version.inc")
    new_version = None
    if os.path.exists(VERSION_FILE):
        print("\n🔢 Aggiornamento versione in version.inc...")
        with open(VERSION_FILE, "r", encoding="utf-8") as f:
            content = f.read()

        match = re.search(r'set\(SoftFever_VERSION\s+"(\d+)\.(\d+)\.(\d+)"\)', content)
        if match:
            major, minor, patch = map(int, match.groups())
            patch += 1  # ✅ incrementa l'ULTIMA cifra
            new_version = f"{major}.{minor}.{patch}"
            new_line = f'set(SoftFever_VERSION "{new_version}")'
            content = re.sub(r'set\(SoftFever_VERSION\s+".*"\)', new_line, content)
            with open(VERSION_FILE, "w", encoding="utf-8") as f:
                f.write(content)
            print(f"  ✅ Versione aggiornata a {new_version}")
        else:
            print("⚠️ Nessuna riga di versione trovata in version.inc")
    else:
        print("⚠️ File version.inc non trovato!")

    # --- 6) Aggiorna JSON ---
    if new_version:
        print("\n🧩 Aggiornamento dei file JSON in resources/profiles...")
        old_pattern = re.compile(r'"version"\s*:\s*"(\d+\.\d+\.\d+)\.\d+"')
        for root, _, files in os.walk(PROFILES_DIR):
            for file in files:
                if file.endswith(".json"):
                    file_path = os.path.join(root, file)
                    try:
                        with open(file_path, "r", encoding="utf-8") as f:
                            content = f.read()
                        new_content = old_pattern.sub(f'"version":"{new_version}.0"', content)
                        if new_content != content:
                            with open(file_path, "w", encoding="utf-8") as f:
                                f.write(new_content)
                            print(f"  Aggiornato: {file_path}")
                    except Exception as e:
                        print(f"  ⚠️ Errore aggiornando {file_path}: {e}")

    print("\n✨ Processo di pulizia completato con successo!")

if __name__ == "__main__":
    main()

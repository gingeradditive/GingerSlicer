import os
import shutil
import subprocess
import fnmatch

# Percorso base del progetto (lo script può essere eseguito da qualsiasi cartella)
BASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# 1) Pulizia della cartella resources/profiles
PROFILES_DIR = os.path.join(BASE_DIR, "resources", "profiles")

# File e cartelle da mantenere
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

# 2) Cancellazione della cartella doc
DOC_DIR = os.path.join(BASE_DIR, "doc")
if os.path.exists(DOC_DIR):
    print("🗑️ Rimuovo la cartella doc...")
    shutil.rmtree(DOC_DIR, ignore_errors=True)

# 3) Lancio dello script ConvertSVGtoGingerColor.py
CONVERT_SCRIPT = os.path.join(BASE_DIR, "scripts", "ConvertSVGtoGingerColor.py")
if os.path.exists(CONVERT_SCRIPT):
    print("🎨 Eseguo ConvertSVGtoGingerColor.py...")
    subprocess.run(["python", CONVERT_SCRIPT], check=False)
else:
    print("⚠️ Script ConvertSVGtoGingerColor.py non trovato!")

print("\n✨ Processo di pulizia completato!")

import os

# Colore da cercare e colore da sostituire
ORCA_GREEN = "009688"
ORCA_DARK_GREEN = "008a79"
ORCA_EXTRA_DARK_GREEN = "225158"

GINGER_RED = "d72828"
GINGER_DARK_RED = "a31e1e"
GINGER_EXTRA_DARK_RED = "8a1919"


def replace_color_in_svg(file_path, ORCA_GREEN, GINGER_RED):
    with open(file_path, 'r', encoding='utf-8') as file:
        content = file.read()

    if ORCA_GREEN.lower() not in content.lower():
        return  # Nessuna sostituzione necessaria

    # Sostituzione case-insensitive
    content_new = content.replace(ORCA_GREEN.lower(), GINGER_RED.lower())
    content_new = content_new.replace(ORCA_GREEN.upper(), GINGER_RED.lower())

    with open(file_path, 'w', encoding='utf-8') as file:
        file.write(content_new)
    print(f"[✓] Modificato: {file_path}")

def convert_svg_colors(base_path):
    for root, dirs, files in os.walk(base_path):
        for file in files:
            if file.endswith(".svg"):
                file_path = os.path.join(root, file)
                replace_color_in_svg(file_path, ORCA_GREEN, GINGER_RED)
                replace_color_in_svg(file_path, ORCA_DARK_GREEN, GINGER_DARK_RED)
                replace_color_in_svg(file_path, ORCA_EXTRA_DARK_GREEN, GINGER_EXTRA_DARK_RED)
                
if __name__ == "__main__":
    base_directory = os.path.dirname(os.path.abspath(__file__))+ "/../"
    convert_svg_colors(base_directory)

import sys

path = r'src/slic3r/GUI/GCodeViewer.cpp'
with open(path, encoding='utf-8') as f:
    src = f.read()

# Strip comments and strings carefully
out = []
i = 0
n = len(src)
in_line_c = False
in_block_c = False
in_str = False
in_chr = False
while i < n:
    c = src[i]
    nxt = src[i+1] if i+1 < n else ''
    if in_line_c:
        if c == '\n':
            in_line_c = False
            out.append(c)
        else:
            out.append(' ')
        i += 1
        continue
    if in_block_c:
        if c == '*' and nxt == '/':
            in_block_c = False
            out.append('  ')
            i += 2
            continue
        out.append(' ' if c != '\n' else '\n')
        i += 1
        continue
    if in_str:
        if c == '\\' and nxt:
            out.append('  ')
            i += 2
            continue
        if c == '"':
            in_str = False
            out.append(' ')
            i += 1
            continue
        out.append(' ' if c != '\n' else '\n')
        i += 1
        continue
    if in_chr:
        if c == '\\' and nxt:
            out.append('  ')
            i += 2
            continue
        if c == "'":
            in_chr = False
            out.append(' ')
            i += 1
            continue
        out.append(' ' if c != '\n' else '\n')
        i += 1
        continue
    if c == '/' and nxt == '/':
        in_line_c = True
        out.append('  ')
        i += 2
        continue
    if c == '/' and nxt == '*':
        in_block_c = True
        out.append('  ')
        i += 2
        continue
    if c == '"':
        in_str = True
        out.append(' ')
        i += 1
        continue
    if c == "'":
        in_chr = True
        out.append(' ')
        i += 1
        continue
    out.append(c)
    i += 1

clean = ''.join(out)
lines = clean.split('\n')

depth = 1  # we are inside function body after the opening {
start = 4347
end = 5704
prev = depth
for ln_idx in range(start-1, min(end, len(lines))):
    line = lines[ln_idx]
    for ch in line:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
    if depth != prev:
        if depth <= 1 or depth < 0:
            print(f"line {ln_idx+1} depth={depth}: {lines[ln_idx].rstrip()[:120]}")
        prev = depth
print(f"Final depth at end of function: {depth}")

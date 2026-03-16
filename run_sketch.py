import os
import sys
import subprocess
import shutil
import tempfile
import time
import re

def preprocess_ino(content):
    # Ensure Arduino.h is included
    if "#include \"Arduino.h\"" not in content and "#include <Arduino.h>" not in content:
        content = "#include \"Arduino.h\"\n" + content
    else:
        content = content.replace("<Arduino.h>", '"Arduino.h"')

    # Extract ALL structs to prevent them being nested or used before defined
    struct_defs = re.findall(r'(struct\s+[a-zA-Z0-9_]+\s*\{.*?\};)', content, re.DOTALL)
    header = ""
    for s in struct_defs:
        if s not in header:
            content = content.replace(s, "")
            header += s + "\n"

    # Forward declarations for all functions
    func_regex = r'\n\s*(?:static\s+)?(?:inline\s+)?(void|int|char|uint8_t|uint16_t|uint32_t|int8_t|int16_t|int32_t|float|double|bool)\s+([a-zA-Z0-9_]+)\s*\(([^)]*)\)\s*\{'
    matches = re.findall(func_regex, '\n' + content)
    declarations = "\n// Forward declarations\n"
    seen = set(['setup', 'loop', 'if', 'while', 'for', 'switch', 'return', 'sizeof', 'min', 'max'])
    for ret, name, args in matches:
        if name not in seen:
            declarations += f"{ret} {name}({args});\n"
            seen.add(name)

    last_inc = -1
    for m in re.finditer(r'#include\s*[<"][^>"]+[>"]', content): last_inc = m.end()
    if last_inc != -1:
        end_line = content.find('\n', last_inc)
        if end_line == -1: end_line = len(content)
        content = content[:end_line+1] + header + declarations + content[end_line+1:]
    else:
        content = header + declarations + content

    return content

def build_sketch(ino_path, output_bin):
    with open(ino_path, 'r') as f: content = f.read()

    # 0. TARGETED SPLITTING to resolve most problematic joins
    keywords = ['void', 'const', 'struct', 'uint8_t', 'uint16_t', 'uint32_t', 'int8_t', 'int16_t', 'int32_t', 'static', 'inline', 'bool', 'int', 'char', 'setup', 'loop', 'Serial', 'delay', 'if', 'for', 'return']
    kw_re = r'|'.join(re.escape(k) for k in keywords)
    content = re.sub(r'([;\}])\s+(' + kw_re + r'|[\{\}])', r'\1\n\2', content)

    lines = content.split('\n')
    new_lines = []
    for line in lines:
        if '//' in line:
            idx = line.find('//')
            before, rest = line[:idx], line[idx:]
            m = re.search(r'(?<=//)(?:.*?)\s+(' + kw_re + r'|[\{\};])', rest)
            if m:
                new_lines.append(before + rest[:m.start(1)])
                new_lines.append(rest[m.start(1):])
                continue
        new_lines.append(line)
    content = '\n'.join(new_lines)

    # Specific known problematic blocks
    content = content.replace("PROGMEM(flash)", "PROGMEM")
    content = content.replace("for USB serial on some boards", "// for USB serial on some boards\n")
    content = content.replace(r'<>/\";', r'<>\\/";')

    # Use clang-format conservatively
    with tempfile.NamedTemporaryFile(suffix=".cpp", mode='w', delete=False) as tmp:
        tmp.write(content)
        tmp_name = tmp.name
    try:
        content = subprocess.check_output(["clang-format", "--style=LLVM", tmp_name], text=True)
    except: pass
    finally: os.remove(tmp_name)

    content = preprocess_ino(content)

    with tempfile.NamedTemporaryFile(suffix=".cpp", mode='w', delete=False) as tmp:
        tmp.write(content)
        tmp_cpp = tmp.name

    mock_dir = os.path.abspath("test_suite/mock_arduino")
    main_cpp = os.path.join(mock_dir, "main.cpp")
    cmd = ["g++", "-x", "c++", "-fpermissive", "-w", "-I", mock_dir, "-I", os.path.join(mock_dir, "avr"), tmp_cpp, main_cpp, "-o", output_bin]
    res = subprocess.run(cmd, capture_output=True, text=True)

    os.remove(tmp_cpp)
    return res.returncode == 0, res.stderr

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 run_sketch.py <sketch.ino>")
        sys.exit(1)

    ino_path = sys.argv[1]
    bin_path = "./sketch_test"

    print(f"Building {ino_path}...")
    ok, err = build_sketch(ino_path, bin_path)
    if not ok:
        print("Build failed:")
        print(err)
        sys.exit(1)

    print(f"Running {ino_path}... (Ctrl+C to stop)")
    try:
        subprocess.run([bin_path])
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if os.path.exists(bin_path): os.remove(bin_path)

if __name__ == "__main__":
    main()

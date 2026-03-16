import os
import sys
import subprocess
import shutil
import tempfile
import time
import re

def build_sketch(ino_path, output_bin):
    mock_dir = os.path.abspath("test_suite/mock_arduino")
    main_cpp = os.path.join(mock_dir, "main.cpp")
    cmd = ["g++", "-x", "c++", "-fpermissive", "-w", "-I", mock_dir, "-I", os.path.join(mock_dir, "avr"), ino_path, main_cpp, "-o", output_bin]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode == 0, res.stderr

def build_and_run_all():
    ino_files = []
    for root, dirs, files in os.walk('.'):
        if 'test_suite' in root: continue
        for f in files:
            if f.endswith('.ino'): ino_files.append(os.path.join(root, f))

    print(f"Found {len(ino_files)} sketches to test.\n")

    passed = []
    failed = []
    for ino in sorted(ino_files):
        print(f"Testing {ino}...", end="", flush=True)
        bin_path = "./sketch_test"
        ok, err = build_sketch(ino, bin_path)
        if ok:
            try:
                proc = subprocess.Popen([bin_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                time.sleep(0.5)
                proc.terminate()
                out, _ = proc.communicate(timeout=1)
                if out:
                    print(" [PASS]")
                    passed.append(ino)
                else:
                    print(" [FAIL: No output]")
                    failed.append(ino)
            except:
                print(" [FAIL: Run error]")
                failed.append(ino)
        else:
            print(" [FAIL: Build error]")
            failed.append(ino)

    print(f"\nSummary: {len(passed)} passed, {len(failed)} failed")
    if os.path.exists("./sketch_test"): os.remove("./sketch_test")

    if failed: sys.exit(1)
    else: sys.exit(0)

if __name__ == "__main__":
    build_and_run_all()

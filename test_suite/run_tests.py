import os
import sys
import subprocess
import time

def build_and_run(ino_path):
    mock_dir = os.path.abspath("test_suite/mock_arduino")
    main_cpp = os.path.join(mock_dir, "main.cpp")

    # Compile the sketch with the mock environment
    cmd = ["g++", "-x", "c++", "-fpermissive", "-w",
           "-I", mock_dir, "-I", os.path.join(mock_dir, "avr"),
           ino_path, main_cpp, "-o", "./sketch_test"]

    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        return False, f"Build error:\n{res.stderr}"

    # Run for a short duration to verify it produces output
    try:
        proc = subprocess.Popen(["./sketch_test"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Give it a bit more time to start up and produce first frames
        time.sleep(0.5)
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=2)
        if stdout:
            return True, f"Passed! Produced {len(stdout)} bytes of output."
        else:
            return False, "Failed: No output produced."
    except Exception as e:
        return False, f"Run error: {e}"

def main():
    ino_files = []
    for root, dirs, files in os.walk('.'):
        if 'test_suite' in root: continue
        for f in files:
            if f.endswith('.ino'):
                ino_files.append(os.path.join(root, f))

    print(f"Found {len(ino_files)} sketches to test.\n")

    passed = []
    failed = []
    for ino in sorted(ino_files):
        print(f"Testing {ino}...", end="", flush=True)
        ok, msg = build_and_run(ino)
        if ok:
            print(" [PASS]")
            passed.append(ino)
        else:
            print(" [FAIL]")
            print(f"  {msg}")
            failed.append(ino)

    print(f"\nSummary: {len(passed)} passed, {len(failed)} failed")

    # Cleanup
    if os.path.exists("./sketch_test"):
        os.remove("./sketch_test")

    if failed:
        sys.exit(1)
    else:
        sys.exit(0)

if __name__ == "__main__":
    main()

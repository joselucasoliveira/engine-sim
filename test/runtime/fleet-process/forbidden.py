import subprocess
import sys

for boundary in ("direct", "shared"):
    for scenario in ("nominal", "zero", "invalid", "stale", "starved", "short", "inject"):
        print(f"cold callback boundary={boundary} case={scenario}", flush=True)
        subprocess.run([sys.argv[1], boundary, scenario, sys.argv[2]], check=True)

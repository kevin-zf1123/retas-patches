"""Build the self-contained canvas display package."""
import subprocess
import sys
from pathlib import Path
if __name__=="__main__":
    tool=Path(__file__).resolve().parents[3]/"tools/build_plugins.py"
    subprocess.run([sys.executable,tool,"--only","anti-alias",*sys.argv[1:]],check=True)

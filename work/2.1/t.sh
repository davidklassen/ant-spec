#!/bin/sh
# Runs one test file through antc under antpy, showing stdout, stderr and the exit status.
cd "$(dirname "$0")/../.."
python3 -I bootstrap/antpy.py run src/antc/main.ant run "$@"
echo "exit=$?"

#!/bin/bash -l
# Launch the isolated parallel RTL backend sweep from the repository environment

set -e
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
source .envrc
exec python analysis/scripts/matrix_backend_sweep.py "$@"

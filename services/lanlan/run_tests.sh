#!/usr/bin/env bash
# Cyber Lanlan service test runner.
#
# Usage (from anywhere):
#   ./services/lanlan/run_tests.sh
#
# The repository gate (tools/validate.sh) should invoke the same suite from the
# repository root with one line:
#
#   PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s services/lanlan/tests -t .
#
# This script is a thin wrapper that fixes the working directory, keeps bytecode
# out of the tree and lowers the PBKDF2 cost so the suite stays fast.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${here}/../.." && pwd)"

cd "${repo_root}"
export PYTHONDONTWRITEBYTECODE=1
export LANLAN_PBKDF2_ITERATIONS="${LANLAN_PBKDF2_ITERATIONS:-1000}"

exec python3 -m unittest discover -s services/lanlan/tests -t . "$@"

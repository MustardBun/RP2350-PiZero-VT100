#!/usr/bin/env bash
#
# Bake the terminal fonts from the sources listed in tools/fonts.json.
#
# Edit tools/fonts.json to try different TTF or BDF files, then run this
# script, then rebuild to produce a new UF2.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 was not found on PATH." >&2
    exit 1
fi

PYTHON=python3

if ! "$PYTHON" -c "import PIL" >/dev/null 2>&1; then
    echo "Pillow not found - installing..."
    "$PYTHON" -m pip install --quiet pillow
fi

"$PYTHON" "$ROOT/tools/gen_fonts.py"

echo ""
echo "Done. Rebuild to pick up the new font tables."

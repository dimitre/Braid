#!/bin/bash
cd "$(dirname "$0")"
set -e

git pull

# Run libs.sh visibly (don't swallow its stdout)
./libs/libs.sh

# Add chalet to PATH if found
CHALET_PATH=$(command -v chalet || true)
if [[ -n "$CHALET_PATH" ]]; then
    export PATH="$(dirname "$CHALET_PATH"):$PATH"
fi

chalet buildrun --only-required feed
# cd ofGen
# ./compile.sh

# cd ../examples/demos/organicText

# if command -v ofgen >/dev/null 2>&1; then
#     ofgen templates=chalet buildrun
# else
#     ../../../ofgen/dist/ofgen templates=chalet buildrun
# fi

# cd ../../..

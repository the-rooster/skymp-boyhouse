#!/usr/bin/env bash
set -euo pipefail
cd /src
node --experimental-strip-types --test misc/boyhouse/*.test.cjs
python3 -m unittest discover -s misc/boyhouse -p 'test_*.py'
# Static coverage remains untested; reports never enable quest adapters.
python3 misc/boyhouse/quest_dependencies.py --data-dir /game-data \
  --archive '/game-data/Skyrim - Misc.bsa' \
  --output /src/build/quest-dependencies.json --markdown /src/build/quest-backlog.md
# Shared packet decoding and projection gates affect ordinary gameplay too.
# Run the complete native suite, including archive and visibility-barrier cases.
./build/unit/unit
node misc/boyhouse/native-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs --projection
node misc/boyhouse/storage-smoke.cjs

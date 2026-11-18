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
node misc/boyhouse/guest-restart-smoke.cjs
# Exercise every non-deleted winning QUST against original records and scripts.
node misc/boyhouse/quest-startup-runner.cjs --catalog build/quest-dependencies.json \
  --output build/quest-startup.json --markdown build/quest-startup.md
# Deliver ADIA to an isolated NPC pair and compare the same winning records.
node misc/boyhouse/quest-startup-runner.cjs --catalog build/quest-dependencies.json \
  --scenario actor-dialogue --contrast build/quest-startup.json \
  --output build/quest-actor-dialogue.json --markdown build/quest-actor-dialogue.md

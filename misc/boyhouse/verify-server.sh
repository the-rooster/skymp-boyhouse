#!/usr/bin/env bash
set -euo pipefail
cd /src
node --experimental-strip-types --test misc/boyhouse/partyInstances.test.cjs \
  misc/boyhouse/instanceCommand.test.cjs misc/boyhouse/questJournalFeed.test.cjs \
  misc/boyhouse/questJournalReplica.test.cjs misc/boyhouse/worldProjection.test.cjs \
  misc/boyhouse/campaignPacketChannel.test.cjs
python3 -m unittest discover -s misc/boyhouse -p 'test_*.py'
# Static coverage remains untested; reports never enable quest adapters.
python3 misc/boyhouse/quest_dependencies.py --data-dir /game-data \
  --archive '/game-data/Skyrim - Misc.bsa' \
  --output /src/build/quest-dependencies.json --markdown /src/build/quest-backlog.md
./build/unit/unit '[Instance],[Quest],[SQLite]'
node misc/boyhouse/native-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs --projection
node misc/boyhouse/storage-smoke.cjs

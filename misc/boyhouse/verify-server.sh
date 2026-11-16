#!/usr/bin/env bash
set -euo pipefail
cd /src
node --test misc/boyhouse/partyInstances.test.cjs
node --test misc/boyhouse/questJournalFeed.test.cjs
python3 -m unittest discover -s misc/boyhouse -p 'test_*.py'
./build/unit/unit '[Instance],[Quest]'
node misc/boyhouse/native-smoke.cjs
node misc/boyhouse/protocol-smoke.cjs

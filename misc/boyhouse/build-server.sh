#!/usr/bin/env bash
set -euo pipefail
cd /src
mkdir -p .vcpkg-cache
./build.sh --configure -DCMAKE_BUILD_TYPE=Release -DBUILD_CLIENT=OFF -DBUILD_SKYRIM_PLATFORM=OFF -DBUILD_SCRIPTS=OFF -DBUILD_UNIT_TESTS=ON -DUNIT_DATA_DIR=/game-data
./build.sh --build --target unit skymp5-server --parallel 3

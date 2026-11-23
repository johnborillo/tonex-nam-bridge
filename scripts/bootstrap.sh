#!/bin/bash
# Fetches third-party sources at the exact commits this project was tested with.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/deps"
fetch() { # url dir commit
  if [ ! -d "$ROOT/deps/$2" ]; then git clone --recurse-submodules "$1" "$ROOT/deps/$2"; fi
  git -C "$ROOT/deps/$2" fetch --quiet origin
  git -C "$ROOT/deps/$2" checkout --quiet "$3"
  git -C "$ROOT/deps/$2" submodule update --init --recursive --quiet
}
fetch https://github.com/sdatkinson/NeuralAmpModelerCore.git NeuralAmpModelerCore d3189ed482ded880c128bf312941266c4fa5ceec   # MIT
fetch https://github.com/gavv/libASPL.git libASPL 633e0f70203edd87d320fc5a3cae901e1363aac5                                    # MIT
echo "deps ready"

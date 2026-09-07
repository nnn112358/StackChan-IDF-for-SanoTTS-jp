#!/usr/bin/env bash
# 端末内漢字 G2P 用の辞書 blob (13.7 MB、git には入れていない) を sanoTTS-jp の Release から取る。
#   tools/get-sano-dict.sh   → assets/sanotts/k1-dict-438750.bin
# `make flash BOARD=cores3-dict` がこれを dict パーティションに書き込む。
set -euo pipefail
cd "$(dirname "$0")/.."
gh release download v0.3.0 --repo ayutaz/sanoTTS-jp --pattern 'k1-dict-438750.bin' --dir assets/sanotts --clobber
echo "f162c922074d76817298b34d8a8fd35f7d195f38540303485a76c956b5d84877  assets/sanotts/k1-dict-438750.bin" | sha256sum -c -

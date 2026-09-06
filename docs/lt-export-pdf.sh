#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# LT スライド (HTML) を指定テーマで PDF に書き出す。
#   usage: docs/lt-export-pdf.sh [theme] [out.pdf]
#   theme: night | lcd | paper | ink | mono | mono-light  (default: night)
# 1 スライド = 1 ページ (1920×1080)。ページ番号オーバーレイは印刷時に消える。
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
theme="${1:-night}"
out="${2:-$here/lt-m5meetup-stackchan-idf-slides-${theme}.pdf}"
chromium="$(command -v chromium || command -v chromium-browser || command -v google-chrome)"
"$chromium" --headless=new --disable-gpu --no-pdf-header-footer \
  --print-to-pdf="$out" \
  "file://$here/lt-m5meetup-stackchan-idf-slides.html?theme=$theme" >/dev/null 2>&1
echo "wrote $out"

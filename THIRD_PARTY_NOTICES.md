# 第三者ソフトウェア・データの帰属表示 (Third-Party Notices)

このリポジトリ (StackChan-IDF-for-SanoTTS-jp) の**自前ソース**は Boost Software License 1.0
([LICENSE](LICENSE)) で配布されます。上流 stackchan-idf 由来のファイルは
© Kenta IDA、このフォークで新規に書いたファイル (`components/jtts/src/sano_*.cpp` /
`resampler.*`、`main/sano_model.*` / `sano_stream_player.*`、`assets/purin_face.avdsl` など) は
© nnn112358 で、いずれも同じ BSL-1.0 です。本ファイルは、firmware / Web フラッシャー配布物 (リリース ZIP・
GitHub Pages) に含まれる、または同梱する**第三者のソフトウェアと音声データ**の
帰属表示と適用ライセンスをまとめたものです。

このページの HTML 版は <https://ciniml.github.io/stackchan-idf/licenses.html>
から参照できます (設定ページ・Web フラッシャーからもリンク)。

---

## 音声合成エンジン

### hts_engine API 1.10 — Modified BSD (3-clause BSD)

HMM 音声合成エンジン。`components/hts_engine/` に組み込み向け改変コピーを同梱
(改変点は [components/hts_engine/README.md](components/hts_engine/README.md) 参照)。

- Copyright (c) 2001-2015 Nagoya Institute of Technology, Department of Computer Science
- Copyright (c) 2001-2008 Tokyo Institute of Technology, Interdisciplinary Graduate School of Science and Engineering
- 上流: <https://hts-engine.sourceforge.net/>
- ライセンス全文: [components/hts_engine/COPYING](components/hts_engine/COPYING)

> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met
> ... THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
> "AS IS" ...

### sanoTTS-jp 推論コア — MIT

蒸留ニューラル TTS [sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) の C99 推論コアと
かな G2P。`components/saanotts_core/` に vendored (origin/main d169e91、無改変。
[components/saanotts_core/README.md](components/saanotts_core/README.md))。

- Copyright (c) 2026 yousan — MIT License
- ライセンス全文: [components/saanotts_core/LICENSE](components/saanotts_core/LICENSE)

### Open JTalk — Modified BSD (cores3-dict のみ)

端末内漢字 G2P に使う Open JTalk のテキスト処理部 (`components/saanotts_core/openjtalk/`、
sanoTTS-jp が pyopenjtalk-plus から取り込んだもの。`jpcommon_label.c` の MAXBUFLEN 変更のみ)。

- Copyright (c) 2008-2016 Nagoya Institute of Technology / HTS Working Group — Modified BSD
- ライセンス全文: [components/saanotts_core/openjtalk/COPYING](components/saanotts_core/openjtalk/COPYING)

### 辞書 `k1-dict-438750.bin` — Modified BSD (cores3-dict のみ、git 非同梱)

NAIST Japanese Dictionary (NAIST) と UniDic (The UniDic Consortium) を sanoTTS-jp が TTS 用に
枝刈り・形式変換した派生物。`tools/get-sano-dict.sh` で取得し `dict` パーティションに書き込む。
帰属表示: [assets/sanotts/NOTICE-dictionary.txt](assets/sanotts/NOTICE-dictionary.txt)。

---

## 音声モデル (sanoTTS-jp 重み)

### sanoTTS-jp モデル v3 (int8) — sanoTTS-jp Model License 1.0

`assets/sanotts/saanotts-jp-v3-int8.bin` (sanoTTS-jp Release v0.3.0、654,032 B、SHA-256
`2d2b8543…`)。CoreS3 の一括イメージ (`firmware-<ver>-cores3.bin`) と `make flash` が
`sano` パーティションに書き込む。**リポジトリの BSL-1.0 / コアの MIT は重みには適用されない。**

- ライセンス: `LicenseRef-sanoTTS-jp-Model-1.0`
  (全文: [assets/sanotts/LICENSE-MODEL.md](assets/sanotts/LICENSE-MODEL.md))
- 帰属表示 (ライセンス §3.1 により**そのまま**再掲) と生成音声の**用途制限** (§3.2:
  個人・団体への攻撃・批判 / 政治・宗教上の主張 / アダルト用途 / 音声素材としての
  再配布に使えない。つくよみちゃんコーパスの条件が伝播したもの) は
  [assets/sanotts/NOTICE.md](assets/sanotts/NOTICE.md)。重みを含むイメージを再配布する
  ときは同 NOTICE ごと配布すること (§3.3)。

---

## 音声モデル (HMM ボイス)

以下は firmware にバンドルするか、Web フラッシャー / 設定ページの「サーバーから
取得」で配布するボイス データです。いずれも Creative Commons 表示ライセンスで、
帰属表示のうえ再配布しています。マニフェスト
[assets/voices.json](assets/voices.json) にも各ボイスの帰属文字列を持たせ、設定
ページのボイス選択 UI に表示します。

### HTS Voice "Mei" — CC BY 3.0

配布ボイス (既定候補)。`assets/voices/mei.htsvoice` として同梱し、GitHub Pages
の `voices/mei.htsvoice` から配布します。

- HTS Voice "Mei" © 2009-2013 Nagoya Institute of Technology / MMDAgent Project Team
- License: [Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/)
- ライセンス全文: [assets/voices/LICENSE_mei.txt](assets/voices/LICENSE_mei.txt)
- 上流: <http://www.mmdagent.jp/>
- **改変版**: `assets/voices/mei16.htsvoice` は上記 Mei を **16 kHz にオフライン
  ダウンサンプル**した派生物 (帯域制限メルケプストラム変換、Kenta IDA 2026)。
  CC BY 3.0 に基づき改変を明示。合成負荷を下げる高速版として同カタログで配布。

### その他の CC BY ボイス (ユーザーがアップロードして利用可能)

firmware は任意の `.htsvoice` を読み込めます。以下はライセンス上クリーンで
検証に用いたボイス (リポジトリには同梱しません)。利用・再配布時は各ライセンスの
帰属表示に従ってください。

- **htsvoice-tohoku-f01** (女声) — CC BY 4.0, © 東北大学 乾・鈴木研究室 (icn-lab)
- **hts_voice_nitech_jp_atr503_m001** (男声) — CC BY 3.0, © Nagoya Institute of Technology

---

## 顔デザインの参照元

### 「プリンを守る技術」の顔 — aNo研 (2019)

`assets/purin_face.avdsl` (このフォークの組み込みデフォルト顔) は
<https://github.com/anoken/purin_wo_mamoru_gijutsu/> の `draw_face()` (真っ赤な背景 — このフォークではシアンに変更、
黒い怒り眉 / 丸い目 / 矩形の口、毎フレームの震え) を参照して **Avatar DSL で書き直した**
もの。参照したのは座標・色・形といった数値の事実だけで、上流のソース コードは
持ち込んでいない (DSL の実装はこのリポジトリのオリジナル、BSL-1.0)。

---

## テキスト解析 (ホスト側ツールのみ)

`tools/jvox/`・`docs/jtts-hmm-research.md` の音声データ生成・検証は、ホスト上で
Open JTalk / pyopenjtalk を使います (firmware には含みません)。

- **Open JTalk / hts_engine / pyopenjtalk** — Modified BSD 等。生成に用いた
  かな→ラベル変換のリファレンスであり、成果物 (.htsvoice / .jvox) の
  ライセンスは元の音声モデルのライセンスに従います。

---

## ライブラリ (submodule / managed components)

| コンポーネント | ライセンス | 著作権表示 |
|---|---|---|
| [M5GFX](components/M5GFX/LICENSE) | MIT | © 2021 M5Stack |
| [M5Unified](components/M5Unified/LICENSE) | MIT | © 2021 M5Stack |
| [tl::expected](components/tl_expected/expected/COPYING) | CC0 1.0 (public domain) | Sy Brand |
| ESP-IDF managed_components (`espressif/*`) | Apache-2.0 | © Espressif Systems |

## Web フラッシャー (GitHub Pages)

| コンポーネント | ライセンス | 著作権表示 |
|---|---|---|
| esptool-js (`docs/esptool.js`) | Apache-2.0 | © Espressif Systems |
| pako (esptool-js に同梱) | MIT AND Zlib | © 2014-2017 Vitaly Puzrin, Andrey Tupitsin |

---

各ライセンスの全文は上記リンク先、および各コンポーネントの LICENSE / COPYING
ファイルを参照してください。

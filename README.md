[English](README.en.md)

# StackChan-IDF-for-SanoTTS-jp

M5Stack CoreS3 の上で日本語を喋る Stack-chan ファームウェア。クラウドにも外部サーバーにも
繋がず、**ニューラル TTS を端末だけで動かして**、その音声に合わせてアバターの口を動かします。

[stackchan-idf](https://github.com/ciniml/stackchan-idf) のフォークで、
[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) (559 K params の蒸留モデル、22.05 kHz)
を音声合成エンジンとして統合したものです。ESP-IDF 5.5 / C++20。

![CoreS3 上で sanoTTS が喋り、口が動く様子](docs/media/demo.gif)

> 実機の動作 ([@nnn112358 の投稿](https://x.com/nnn112358/status/2097082230556467251))。
> `POST /api/jtts-say` に送った文を端末だけで合成し、吹き出しに出しながら喋っています。

```
読み (かな or 漢字かな交じり)
   → G2P (かな中間表現 / Open JTalk + 辞書)
   → sanoTTS 推論 (W8A8 + ESP32-S3 の整数 SIMD、作業領域 176 KB)
   → 22.05 kHz PCM をチャンクごとに再生 (合成しながら鳴らす)
   → 包絡からアバターの口を駆動
```

## できること

- **`POST /api/jtts-say` に文を投げるだけで喋る。** 辞書入りビルドなら漢字かな交じり文をそのまま
- **合成しながら鳴らす。** 先読み量は前回実測の実時間比 (xRT) から自動で決まる
- **リップシンク。** 再生位置の音量でアバターの口が開閉する
- **首も動かせる。** `POST /api/servo-pose` で発話の前後におじぎ・首振り
- 上流の機能 (AI 音声対話、BLE / Wi-Fi / SoftAP 設定、OTA、Avatar DSL、ダンス、LT タイマー、
  NeoPixel、4 ボード対応) はそのまま

## クイックスタート

ESP-IDF 5.5 を導入済みの環境で:

```sh
git clone https://github.com/nnn112358/StackChan-IDF-for-SanoTTS-jp
cd StackChan-IDF-for-SanoTTS-jp
git submodule update --init --recursive
tools/apply-m5-patches.sh                             # M5Unified への小さな修正

make set-target BOARD=cores3
make build      BOARD=cores3
make flash      BOARD=cores3 PORT=/dev/ttyACM0
```

漢字をそのまま喋らせたい場合は辞書入りプロファイルを使います (下記「ビルド プロファイル」)。

```sh
tools/get-sano-dict.sh                                # 辞書 13.7 MB を取得 (gh CLI が要る)
make set-target BOARD=cores3-dict
make build      BOARD=cores3-dict
make flash      BOARD=cores3-dict PORT=/dev/ttyACM0   # 辞書込みで 2 分半ほど
```

モデルの重み (654 KB) はリポジトリに同梱していて `make flash` が書き込みます。
ESP-IDF のパスは `IDF_PATH=`、シリアル ポートは `PORT=` で上書きできます。

Wi-Fi は BLE 設定ページ / SoftAP の captive portal / `tools/ble-cli` のいずれかで設定します。

## 使い方

```sh
# 喋る (辞書入りビルドなら漢字のまま)
curl -X POST --data-binary "今日は良い天気ですね。" http://<device>/api/jtts-say

# 発話の前後に首を動かす
curl -X POST -d '{"yaw":25,"time_ms":400}'          http://<device>/api/servo-pose
curl -X POST -d '{"yaw":0,"pitch":0,"time_ms":400}' http://<device>/api/servo-pose

# 状態
curl http://<device>/api/sano-model    # {"loaded":true,"dict":true,"capacity":655360}
```

### 読みの書き方

かなに加えて、sanoTTS の中間表現をそのまま書けます。

| 記号 | 意味 |
|---|---|
| `[` | アクセント上昇 |
| `]` | 下降核 (jtts の `'` も同じ扱い) |
| `#` | 句境界 (`、` `。` `/` 空白も同じ) |
| `°` | 無声化 |
| `?` `?!` `?.` `?~` | 疑問 |

例: `きょ][おわよ][いて][んきです°ね` = 「今日は良い天気ですね」。
辞書入りビルドなら漢字のまま送れば、読みとアクセントは辞書が決めます。

### 声の設定

`POST /api/jtts-config` に JSON で渡します (設定ページからも変更できます)。

| フィールド | 値 |
|---|---|
| `engine` | `auto` (既定、sanoTTS 優先) / `sano` / `hmm` / `unit` / `formant` |
| `mora_ms` | 話速。110 が等速、大きいほどゆっくり |
| `gain` | 音量 (既定 0.6 に対する相対) |

話速はビルド時にも一律の補正が掛かります (`CONFIG_JTTS_SANO_SPEED_PCT`、既定 125 =
継続長 1.25 倍)。学習モデルがそのままだと速めなためです。

## ビルド プロファイル

| | `cores3` (既定) | `cores3-dict` |
|---|---|---|
| 入力 | かな + アクセント記号 | **かな + 漢字かな交じり文** |
| flash 構成 | OTA 2 面 (4 MB × 2) + sano 1 MB | 単一アプリ 2.19 MiB + sano + **辞書 13.1 MiB** |
| アプリ サイズ | 3.68 MB (残り 12 %) | 2.08 MB (残り 9 %) |
| OTA | あり | なし (更新は USB) |
| 会話 / オーディオ ストリーム / カメラ / ASR / HMM / ESP-NOW | あり | なし |
| 日本語フォント | 12 / 16 / 20 / 24 px | 16 px のみ |

パーティション表は [partitions_16mb.csv](partitions_16mb.csv) と
[partitions_16mb_dict.csv](partitions_16mb_dict.csv)。表が変わるので、**プロファイルを移る
ときは USB でフル書き込み**が要ります。辞書は容量が大きいため git に入れず、
`tools/get-sano-dict.sh` で取得します。

## このフォークで追加したもの

| 追加点 | 内容 |
|---|---|
| **sanoTTS エンジン** | `jtts::Engine::Sano`。既存の formant / 単位連結 / HMM に並ぶ 4 つ目のエンジンで、`auto` では最優先。合成結果は上流の記録値と bit 一致 |
| **ストリーミング再生** | 合成しながら鳴らす。先読み量は前回実測の xRT から自動 ([main/sano_stream_player.cpp](main/sano_stream_player.cpp)) |
| **リップシンク** | 16 ms 窓のピーク包絡を発話内の最大値で正規化し、再生位置から 10 ms ごとに口の開きを更新 |
| **端末内漢字 G2P** | `BOARD=cores3-dict`。Open JTalk + 枝刈り辞書 13.7 MB で読みとアクセントを付ける |
| **話速の補正** | `CONFIG_JTTS_SANO_SPEED_PCT` (既定 125 = 継続長 1.25 倍)。モデルが速めなので既定でゆっくりに |
| **`POST /api/servo-pose`** | 首の姿勢を度で直接指定。発話の前後におじぎ・首振りをさせる入口 |
| **顔プリセット `purin`** | aNo研「[プリンを守る技術](https://github.com/anoken/purin_wo_mamoru_gijutsu/)」ベースの顔 ([assets/purin_face.avdsl](assets/purin_face.avdsl))。設定ページのプリセット / `POST /api/avatar-dsl` で切り替え、組み込みデフォルトは `CONFIG_AVATAR_DEFAULT_FACE` |

## このフォークで無効化したもの


### 通常ビルド (`cores3`) で無効化したもの

| 無効化したもの | 理由 / 影響 |
|---|---|
| esp-sr モデル領域 2.9 MB → **1.9 MB** | sanoTTS の重み用に 1 MB を譲った。オフセットは据え置きなので書き込み済みモデルはそのまま有効。日本語ウェイクワード 1 語 (`srmodels.bin` 約 290 KB) には十分 |
| 起動時の 440 Hz 診断音 | 16 kHz 再生経路のブリングアップ用に毎回鳴っていたビープ。既定で鳴らさない (`kBootPlayRawProbe`) |
| 起動音 (アルペジオ) の既定 ON | 既定 OFF に変更。設定ページ / BLE で ON にできる |

機能そのものは何も無効化していません (会話・OTA・カメラ・ASR・HMM・ESP-NOW すべて動きます)。

ただし **`engine: "auto"` の優先順位が変わりました**: sanoTTS → HMM → 単位連結 → フォルマント。
sanoTTS のモデルは同梱・書き込み済みなので、通常ビルドでも**既定では HMM や音声 DB の声は
選ばれません** (HMM ボイスと `voice` パーティション 4 MB はそのまま残っています)。従来の声に
戻すには `POST /api/jtts-config` で `{"engine":"hmm"}` のように明示します。

### 辞書入りビルド (`cores3-dict`) で無効化したもの

辞書 13.7 MB を載せるため、flash を単一アプリ 2.19 MiB に切り詰めた結果です。

| 無効化した機能 | 影響する API |
|---|---|
| OTA (アプリ 2 面 → 1 面) | `/api/ota/*`、`/api/release/versions` |
| AI 音声対話 (OpenAI / Gemini / XiaoZhi) | 会話タブ、barge-in |
| BLE / Wi-Fi オーディオ ストリーム (AAC コーデック込み) | RTP 受信、BLE 音声 |
| カメラ (GC0308) と QR | `/api/camera/*` |
| オンデバイス ASR (esp-sr WakeNet) | ウェイクワード起動 |
| HMM 合成 (hts_engine)、単位連結の音声 DB、`voice` パーティション 4 MB | `/api/hmm-voice/*`、`/api/voice-db`、`/api/voices` |
| ESP-NOW リモコン | ESP-NOW の動作モード |
| 日本語フォント 12 / 20 / 24 px | 表示は 16 px に統一 (約 570 KB 節約) |

顔・サーボ・Wi-Fi 設定ページ・BLE 設定・LT タイマー・ダンス・sanoTTS は残っています。

## 仕組み

| 追加した部分 | 場所 |
|---|---|
| 推論コア + かな G2P + 漢字 G2P (上流からの vendored、MIT) | [components/saanotts_core](components/saanotts_core/README.md) |
| エンジン本体 (一括合成 + ストリーミング) | `components/jtts/src/sano_synth.cpp` |
| 読み → sanoTTS かな中間表現 | `components/jtts/src/sano_text.cpp` |
| 有理比 sinc リサンプラ | `components/jtts/src/resampler.cpp` |
| 重み / 辞書パーティションの mmap と登録 | `main/sano_model.cpp` |
| ストリーミング再生 + リップシンク | `main/sano_stream_player.cpp` |
| 重み blob・NOTICE・モデル ライセンス | `assets/sanotts/` |

再生は上流 [SanoTTS-jp-M5StackCoreS3](https://github.com/nnn112358/SanoTTS-jp-M5StackCoreS3)
と同じ方式です。発話ぶんのバッファを PSRAM に 1 本取り、先読み量 (音声長 × (1 − 1/xRT) +
2 チャンク) が貯まったら「貯めたぶん」と「残り全部」を渡し、以後は合成が書き足すだけ。
リップシンクは 16 ms 窓のピーク包絡を発話内の最大値で正規化し、再生位置から 10 ms ごとに
口の開きを更新します。

### 実測 (CoreS3、作業領域は PSRAM)

| 項目 | 値 |
|---|---|
| 定常 xRT (合成時間 / 音声長) | 1.7 |
| 発話開始まで (2.5 秒の文) | 3.3 秒 (先読み 57 %) |
| 途切れ | 0 回 |
| 出力 PCM の FNV-1a | `0xa69a7ebbb5ccb05f` — 上流 sanoTTS-jp の記録値と一致 |

作業領域 176 KB は既定で PSRAM に取ります。`CONFIG_JTTS_SANO_ARENA_INTERNAL` で内部 DRAM を
優先させると速くなりますが、この構成では空きが足りません (上流の顔なし構成では xRT 0.45)。

## ハードウェア

- M5Stack CoreS3 (ESP32-S3、8 MB Quad-SPI PSRAM、16 MB Flash)
- Stack-chan ベース: PY32 IO Expander @ 0x6F、SCS0009 ×2 (UART1 TX G6 / RX G7、1 Mbps)、
  頭タッチ センサー Si12T @ 0x68
  - Takao ベースはサーボが Port A (TX G2 / RX G1、半二重)、サーボ電源は外部供給
  - Yaw ID = 1 / Pitch ID = 2、1 step ≈ 0.3125°
- AtomS3R / AtomS3 / StopWatch でもビルドできますが、sanoTTS は 16 MB flash + PSRAM が要るため
  CoreS3 向けの機能です

## ライセンス

このリポジトリの自前ソースは上流と同じ **Boost Software License 1.0**
([LICENSE](LICENSE)) です。追加分には次の第三者成果物が含まれます。

| | ライセンス |
|---|---|
| sanoTTS-jp 推論コア (`components/saanotts_core`) | MIT (© 2026 yousan) |
| **モデルの重み** `assets/sanotts/saanotts-jp-v3-int8.bin` | **sanoTTS-jp Model License 1.0** |
| Open JTalk (`components/saanotts_core/openjtalk`) | Modified BSD |
| 辞書 `k1-dict-438750.bin` (非同梱) | Modified BSD (NAIST-jdic / UniDic 由来) |

> ⚠️ **書き込むイメージにはモデルの重みが含まれます。** 再配布するときは
> [assets/sanotts/NOTICE.md](assets/sanotts/NOTICE.md) ごと配布し、**帰属表示**と
> **生成音声の用途制限** (個人・団体への攻撃・批判 / 政治・宗教上の主張 / アダルト用途 /
> 音声素材としての再配布に使えない。つくよみちゃんコーパスの条件が伝播したもの) を
> 受け取った側にも課してください。

第三者コンポーネント全体の帰属表示は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) に
まとめています。

## 謝辞

- [ayutaz/sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) — モデルと推論コア
- [nnn112358/SanoTTS-jp-M5StackCoreS3](https://github.com/nnn112358/SanoTTS-jp-M5StackCoreS3) — CoreS3 への移植 (スピーカー / 辞書 / リップシンクの設計を参考にしました)
- [ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf) — ベースのファームウェア
- [anoken/purin_wo_mamoru_gijutsu](https://github.com/anoken/purin_wo_mamoru_gijutsu/) — `purin` 顔プリセットの元デザイン

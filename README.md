[English (upstream doc)](README.en.md)

# StackChan-IDF-for-SanoTTS-jp

**[stackchan-idf](https://github.com/ciniml/stackchan-idf) に日本語ニューラル TTS
[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) を載せたフォーク。**
M5Stack CoreS3 の上で、クラウドにも外部サーバーにも繋がずに日本語を喋り、
その音声に合わせてアバターの口が動きます。ESP-IDF 5.5 / C++20。

- 559 K params の蒸留モデル (22.05 kHz) を **flash のパーティションから mmap** して実行
- 合成しながら鳴らす**ストリーミング再生**。包絡からアバターの口を駆動 (リップシンク)
- **漢字かな交じり文をそのまま**喋れる辞書入りビルド (Open JTalk + 13.7 MB 辞書)
- 発話は `POST /api/jtts-say` の 1 本で完結。首の姿勢も `POST /api/servo-pose` で動かせる

上流の機能 (AI 音声対話、BLE / Wi-Fi / SoftAP 設定、OTA、Avatar DSL、ダンス、LT タイマー、
NeoPixel、4 ボード対応) はそのまま残しています。上流の詳細は
[ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf) を参照してください。

---

## このフォークで追加したもの

| 追加点 | 内容 |
|---|---|
| **sanoTTS エンジン** | `jtts::Engine::Sano`。既存の formant / 単位連結 / HMM に並ぶ 4 つ目のエンジンで、`auto` では最優先。推論コアは [components/saanotts_core](components/saanotts_core/README.md) に vendored (MIT) |
| **ストリーミング再生** | [main/sano_stream_player.cpp](main/sano_stream_player.cpp)。先読み量を前回実測の xRT から決め、貯まった時点で鳴らし始める (上流 SanoTTS-jp-M5StackCoreS3 と同じ方式) |
| **リップシンク** | 16 ms 窓のピーク包絡を発話内の最大値で正規化し、再生位置から口の開きを 10 ms ごとに更新 |
| **漢字 G2P (辞書入りビルド)** | `BOARD=cores3-dict`。Open JTalk + 枝刈り辞書で読みとアクセントを付ける |
| **話速の補正** | `CONFIG_JTTS_SANO_SPEED_PCT` (既定 125 = 継続長 1.25 倍)。モデルが速めなので既定でゆっくりにしてある |
| **`POST /api/servo-pose`** | 首の姿勢を度で直接指定。発話の前後におじぎ・首振りをさせる入口 |
| **顔プリセット `purin`** | aNo研「[プリンを守る技術](https://github.com/anoken/purin_wo_mamoru_gijutsu/)」ベースの顔 ([assets/purin_face.avdsl](assets/purin_face.avdsl)) |
| **起動音** | ドレミファソ (C5–D5–E5–F5–G5)、**既定 OFF**。フェード付き正弦波で鳴らして歪みを避ける |

## 2 つのビルド プロファイル

| | `cores3` (既定) | `cores3-dict` |
|---|---|---|
| 入力 | かな + アクセント記号 | **かな + 漢字かな交じり文** |
| flash 構成 | OTA 2 面 (4 MB × 2) + sano 1 MB | 単一アプリ 2.19 MiB + sano + **辞書 13.1 MiB** |
| アプリ サイズ | 3.68 MB (残り 12 %) | 2.08 MB (残り 9 %) |
| OTA | あり | なし (更新は USB) |
| 会話 / オーディオ ストリーム / カメラ / ASR / HMM / ESP-NOW | あり | なし |
| 日本語フォント | 12 / 16 / 20 / 24 px | 16 px のみ |

パーティション表は [partitions_16mb.csv](partitions_16mb.csv) と
[partitions_16mb_dict.csv](partitions_16mb_dict.csv)。表が変わるので、**プロファイルを
移るときは USB でフル書き込み**が要ります。

## セットアップ

ESP-IDF 5.5 を導入済みの環境で:

```sh
git clone https://github.com/nnn112358/StackChan-IDF-for-SanoTTS-jp
cd StackChan-IDF-for-SanoTTS-jp
git submodule update --init --recursive
tools/apply-m5-patches.sh          # M5Unified への小さな修正を適用

# 通常ビルド (かな入力、OTA あり)
make set-target BOARD=cores3
make build      BOARD=cores3
make flash      BOARD=cores3 PORT=/dev/ttyACM0

# 辞書入りビルド (漢字をそのまま喋る)
tools/get-sano-dict.sh             # 辞書 13.7 MB を取得 (git 非同梱、gh CLI が要る)
make set-target BOARD=cores3-dict
make build      BOARD=cores3-dict
make flash      BOARD=cores3-dict PORT=/dev/ttyACM0   # 辞書込みで 2 分半ほど
```

モデルの重み `assets/sanotts/saanotts-jp-v3-int8.bin` (654 KB) はリポジトリに同梱していて、
`make flash` が `sano` パーティションに書き込みます (**OTA では更新されません**)。

`make` が使う ESP-IDF のパスは `IDF_PATH=`、シリアル ポートは `PORT=` で上書きできます。

## 使い方

Wi-Fi を設定すると HTTP API が使えます (BLE 設定ページ、SoftAP の captive portal、
または `tools/ble-cli`)。

```sh
# 喋る (辞書入りビルドなら漢字のまま)
curl -X POST --data-binary "今日は良い天気ですね。" http://<device>/api/jtts-say

# 発話の前後に首を動かす
curl -X POST -d '{"yaw":25,"time_ms":400}'          http://<device>/api/servo-pose
curl -X POST -d '{"yaw":0,"pitch":0,"time_ms":400}' http://<device>/api/servo-pose

# 状態
curl http://<device>/api/sano-model    # {"loaded":true,"dict":true,"capacity":655360}
```

**読みの書き方**: かなに加えて、sanoTTS の中間表現をそのまま書けます。

| 記号 | 意味 |
|---|---|
| `[` | アクセント上昇 |
| `]` | 下降核 (jtts の `'` も同じ扱い) |
| `#` | 句境界 (`、` `。` `/` 空白も同じ) |
| `°` | 無声化 |
| `?` `?!` `?.` `?~` | 疑問 |

例: `きょ][おわよ][いて][んきです°ね` = 「今日は良い天気ですね」。辞書入りビルドなら
漢字のまま送れば、読みとアクセントは辞書が決めます。

声の設定 (`POST /api/jtts-config`) では `engine` (`auto` / `sano` / `hmm` / `unit` /
`formant`)、`mora_ms` (話速)、`gain` (音量) などを変えられます。

## 実測 (M5Stack CoreS3、作業領域は PSRAM)

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

## リポジトリ構成 (フォークで増えたもの)

```
components/saanotts_core/   sanoTTS-jp 推論コア + かな G2P + 漢字 G2P (vendored, MIT)
components/jtts/src/
    sano_synth.cpp          Engine::Sano 本体 (一括 + ストリーミング)
    sano_text.cpp           読み → sanoTTS かな中間表現
    resampler.cpp           有理比 sinc リサンプラ
main/
    sano_model.cpp          重み / 辞書パーティションの mmap と登録
    sano_stream_player.cpp  ストリーミング再生 + リップシンク
assets/sanotts/             重み blob、NOTICE、モデル ライセンス (辞書は非同梱)
partitions_16mb_dict.csv    辞書入り単一アプリの表
sdkconfig.defaults.cores3-dict
```

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

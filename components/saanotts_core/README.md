# saanotts_core — sanoTTS-jp 推論コア (vendored)

[sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp) (559 K params の日本語 TTS、
arXiv:2608.21378 の日本語版) の C99 推論コアと端末側 G2P のコピー。MIT
([LICENSE](LICENSE)、© 2026 yousan)。

| ファイル | 役割 |
|---|---|
| `saanotts.c` / `saanotts_stream.c` / `fft.c` / `saanotts_int8.c` | 推論コア (Duration → Acoustic → iSTFT Decoder、W8A8 + ESP32-S3 PIE) |
| `g2p.c` / `g2p_table.h` | かな中間表現 → 音素 ID 列 |
| `saan_port_esp32.h` | 配置属性 (erf 表を内部 DRAM に) |

出所: sanoTTS-jp `csrc/` origin/main d169e91 (2026-09-04)。
[nnn112358/SanoTTS-jp-M5StackCoreS3](https://github.com/nnn112358/SanoTTS-jp-M5StackCoreS3)
経由で取り込んだ。**ソースは無改変** (ビルド設定だけこのリポジトリのもの)。

入力は**かな中間表現**のみ (ひらがな + `[` 上昇 / `]` 下降核 / `#` 句境界 / `°` 無声化 /
`?` 疑問)。上流の端末内漢字 G2P (Open JTalk + 辞書 13.7 MB) は 16 MB flash の
OTA 構成に収まらないので持ってこない。

重み blob (`assets/sanotts/saanotts-jp-v3-int8.bin`、blob v2、654,032 B) は
**sanoTTS-jp Model License 1.0** — [assets/sanotts/LICENSE-MODEL.md](../../assets/sanotts/LICENSE-MODEL.md)
と [assets/sanotts/NOTICE.md](../../assets/sanotts/NOTICE.md) を参照。
利用側は [components/jtts/src/sano_synth.cpp](../jtts/src/sano_synth.cpp)。

# sanoTTS-jp モデル v3 (int8) — 帰属表示

`saanotts-jp-v3-int8.bin` は [sanoTTS-jp](https://github.com/ayutaz/sanoTTS-jp)
Release **v0.3.0** の `saanotts-jp-v3-int8.bin` (blob v2 形式、654,032 B、
SHA-256 `2d2b8543c06b6a749f19c9918de68244409e2bb6ad1d921a90b5c358f96d4d79`) そのもの。

ライセンス: `LicenseRef-sanoTTS-jp-Model-1.0` (全文: [LICENSE-MODEL.md](LICENSE-MODEL.md))。
**リポジトリの BSL-1.0 / コアの MIT はモデルの重みには適用されない。**

⚠️ CoreS3 向け firmware の一括イメージ (`firmware-<ver>-cores3.bin`) と `make flash` は
この重みを `sano` パーティションに書き込む。再配布するときは本 NOTICE ごと配布し、
下記 §1 の帰属表示と §2 の用途制限を受け取った側にも課すこと (ライセンス §3.3)。

## 1. 帰属表示 (ライセンス §3.1 により**そのまま**再掲。1 行も削らないこと)

```
This model was distilled from a piper-plus teacher model.
sanoTTS-jp — https://github.com/ayutaz/sanoTTS-jp

つくよみちゃんコーパス
  本ソフトウェアの音声合成には、フリー素材キャラクター「つくよみちゃん」
  （© 夢前黎）が無料公開している音声データを使用しています。
  https://tyc.rei-yumesaki.net/material/corpus/

MOE-Speech (litagin) — https://huggingface.co/spaces/litagin/moe-speech-license
  著作権法 30 条の 4（情報解析のための利用）に基づき学習に使用。

蒸留に使用したテキストコーパス:
  - Common Voice ja (Mozilla) — CC0-1.0
      https://github.com/common-voice/common-voice
  - ROHAN4600 (森勢将雅) — CC0-1.0
      https://github.com/mmorise/rohan4600
  - ITA コーパス — CC0-1.0
      https://github.com/mmorise/ita-corpus
  - JSUT ver1.1 (高道慎之介) — CC-BY-SA-4.0 ほか（subset 別）
      https://sites.google.com/site/shinnosuketakamichi/publication/jsut

教師実装: piper-plus (MIT) — https://github.com/ayutaz/piper-plus
```

## 2. 生成音声の用途制限 (ライセンス §3.2。つくよみちゃんコーパスの条件が伝播したもの)

本モデルが生成した音声は次に使えない:
個人・団体への攻撃・批判 / 政治・宗教上の主張 / アダルト用途 / 音声素材としての再配布。
一次ソース: <https://tyc.rei-yumesaki.net/material/corpus/> (食い違う場合は一次ソースが優先)。

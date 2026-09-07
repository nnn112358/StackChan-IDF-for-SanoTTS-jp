// SPDX-FileCopyrightText: 2026 @nnn112358
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS のストリーミング再生 — 合成しながら鳴らす。
//
// SanoTTS-jp-M5StackCoreS3 (saan_speaker.cpp / main.c synth_once) の方式:
//   * 発話バッファ (総サンプル数は begin() で確定) を PSRAM に 1 本取り、ゼロ埋め
//   * チャンクを pull して追記。先読み量に達したら「貯めたぶん」と「残り全部 (まだ
//     書いていない部分を含む)」の 2 区間を M5.Speaker.playRaw に渡す。playRaw は
//     コピーしないので、以後は合成が再生より先を書き続けるだけで鳴り続ける
//   * 先読み量 P は前回の実測 xRT から P ≥ T·(1 − 1/xRT) + 2 チャンク (xRT < 1 なら
//     2 チャンクだけ)。見込みが甘くて追い越されると、その区間はゼロ埋め = 無音
//   * リップシンク: 16 ms 窓のピーク包絡を書きながら作り、10 ms 周期の小タスクが
//     「鳴らし始めた時刻 + 経過時間」から再生位置を推定して包絡を読む。包絡は
//     ここまでの最大値で正規化 (上流と同じ)
//
// 呼び出し元は main/settings_sinks.cpp の say worker。sanoTTS が使えないとき
// (モデル無し / 読めない / 長すぎる) は false を返すので、従来の一括経路に落とす。
#pragma once

#include <string_view>

#include <jtts/jtts.hpp>

namespace stackchan::app {

class SharedState;

// 合成 + 再生を最後まで行い (ブロッキング)、鳴り終わってから戻る。
// state が非 null なら face.mouth_open を駆動する。
bool speak_streaming(std::u32string_view kana, const jtts::Options& opt, SharedState* state);

} // namespace stackchan::app

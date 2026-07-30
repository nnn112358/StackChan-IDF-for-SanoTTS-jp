// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// ダンス エンジン: 音声再生と同期してサーボを駆動する。制御データ = キーフレーム列
// (開始からの時刻 / 移動にかける時間 / yaw・pitch のパーセンテージ)。音声再生の
// 経過時間を master clock にして、各キーフレーム時刻でサーボ目標角を更新する。
// 電源レール競合 (音声劣化) は P0 で許容と確認済み (docs/dance-feature-research.md)。
//
// トリガ (画面 / BLE / HTTP) は dance_control(cmd,id) で SharedState.dance に積み、
// エンジン タスクが消費する。P2 時点の曲は埋め込みサンプル (生成 PCM メロディ)。

#pragma once

#include <cstddef>
#include <cstdint>

#include "servo_limits.hpp"

namespace stackchan::app {

class SharedState;

// 制御データ 1 キーフレーム。フラッシュ blob と同じ 12 バイト レイアウト。
// yaw/pitch は符号付きパーセンテージ ×100 (例 +5000 = +50.00%)。0% = キャリブ
// 中心、+100% = 軸 max、-100% = 軸 min。
struct DanceKeyframe {
    std::uint32_t time_ms;       // 開始からの経過時間
    std::uint16_t move_ms;       // この座標へ移動にかける時間
    std::int16_t yaw_pct_x100;   // 目標 yaw%  ×100
    std::int16_t pitch_pct_x100; // 目標 pitch% ×100
    std::uint16_t flags;         // 予約 (表情/LED 同期などに拡張)
};
static_assert(sizeof(DanceKeyframe) == 12, "keyframe wire layout is 12 bytes");

// フラッシュ / アップロード blob の先頭ヘッダ (48 バイト固定, little-endian)。
// レイアウト: [Header 48B][DanceKeyframe × kf_count][audio audio_len B]。
// tools/dance/build_dance.py が生成する。
struct DanceBlobHeader {
    std::uint32_t magic;        // 'DNC1' = 0x31434E44
    std::uint16_t version;      // 1
    std::uint16_t kf_count;     // キーフレーム数
    std::uint8_t audio_fmt;     // 0 = PCM16LE, 1 = AAC(ADTS) [将来]
    std::uint8_t channels;      // 1
    std::uint16_t reserved0;
    std::uint32_t sample_rate;  // 音声サンプル レート [Hz]
    std::uint32_t audio_offset; // blob 先頭からの音声バイト オフセット (=48+kf_count*12)
    std::uint32_t audio_len;    // 音声バイト数
    std::uint32_t total_ms;     // 曲全体の長さ [ms]
    char name[16];              // 曲名 (nul 詰め)
    std::uint32_t reserved1;
};
static_assert(sizeof(DanceBlobHeader) == 48, "dance blob header is 48 bytes");
inline constexpr std::uint32_t kDanceMagic = 0x31434E44u;  // "DNC1"

// エンジンを起動する (ポーリング タスクを 1 本立てて即戻る)。servo タスク稼働
// 前提。以後 dance_control() でトリガする。
void start_dance_engine(SharedState& state, const ServoLimits& limits);

// トリガ受け口 (画面 / BLE chr / HTTP から呼ぶ)。cmd: 1=start, 2=stop。
// id = 曲選択 (現状は 0 のみ)。SharedState.dance に積むだけで即戻る。
void dance_control(std::uint8_t cmd, std::uint8_t id);

// アップロード受け口 (HTTP /api/dance/upload)。blob をフラッシュへ保存する。
bool dance_upload(const std::uint8_t* data, std::size_t len);

}  // namespace stackchan::app

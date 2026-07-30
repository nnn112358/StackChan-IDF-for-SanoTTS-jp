// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "servo_limits.hpp"

namespace stackchan::app {

class SharedState;

// [Dance P0] 電源レール競合の実機確認: masking を無効化して melody を鳴らしながら
// サーボを踊らせ、音声劣化の許容度を耳で判定する。CONFIG_STACKCHAN_DANCE_POC
// 無効時は no-op。servo タスク起動済み前提。別タスクを起こして即戻る。
void dance_poc_run(SharedState& state, const ServoLimits& limits);

}  // namespace stackchan::app

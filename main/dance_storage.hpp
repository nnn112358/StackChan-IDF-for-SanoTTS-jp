// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// ダンス blob (制御データ + 音声) をフラッシュに保存/ロードする。既存 "storage"
// パーティションの NVS に "dance" 名前空間で置く (voice_db / avatar_vm と別名前空間で
// 共存, パーティション表変更なし)。アップロードは HTTP /api/dance/upload。

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stackchan::app::dance_storage {

// blob を保存する (妥当性を検証してから NVS へ)。true=成功。
bool save(const std::uint8_t* data, std::size_t len);

// 保存済み blob を PSRAM に読み出す (無ければ空)。呼び出し側が保持する。
std::vector<std::uint8_t> load();

}  // namespace stackchan::app::dance_storage

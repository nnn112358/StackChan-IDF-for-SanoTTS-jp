// SPDX-FileCopyrightText: 2026 @nnn112358
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS-jp の重み blob (assets/sanotts/saanotts-jp-v3-int8.bin) のロード。
// 置き場は "sano" パーティション (partitions_16mb.csv、raw、ヘッダ無し — `make flash` /
// 一括イメージが書き込む)。パーティションを esp_partition_mmap して jtts::set_sano_model
// にゼロコピーで渡す。パーティションが無いボード / CONFIG_JTTS_ENABLE_SANO 無効では
// init が false を返し、jtts は他エンジンへフォールバックする。
#pragma once

#include <cstdint>

namespace stackchan::app::sano_model {

// ブート時: sano パーティションを mmap して jtts に登録する。戻り値はロード成功。
bool init();

struct Status {
    bool loaded = false;         // jtts に登録済みか
    bool dict = false;           // 端末内漢字 G2P の辞書が使えるか (cores3-dict)
    std::uint32_t capacity = 0;  // パーティション容量 (0 = パーティションなし)
};
Status status();

}  // namespace stackchan::app::sano_model

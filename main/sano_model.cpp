// SPDX-FileCopyrightText: 2026 nnn112358 <neko112358@gmail.com>
// SPDX-License-Identifier: BSL-1.0
#include "sano_model.hpp"

#include "sdkconfig.h"

#include <esp_log.h>
#include <esp_partition.h>

#include <jtts/jtts.hpp>

#if defined(CONFIG_JTTS_SANO_KANJI)
extern "C" {
#include "saan_dict.h"
}
#endif

namespace stackchan::app::sano_model {

namespace {

constexpr const char* kTag = "sano-model";
constexpr const char* kPart = "sano";

const esp_partition_t* g_part = nullptr;
esp_partition_mmap_handle_t g_mmap = 0;
#if defined(CONFIG_JTTS_SANO_KANJI)
jdict_t g_dict{};
bool g_dict_ok = false;
#endif

const esp_partition_t* find_partition() {
    if (g_part == nullptr) {
        g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPart);
    }
    return g_part;
}

}  // namespace

bool init() {
#if !defined(CONFIG_JTTS_ENABLE_SANO)
    ESP_LOGI(kTag, "sanoTTS engine disabled in this build (CONFIG_JTTS_ENABLE_SANO)");
    return false;
#else
    const esp_partition_t* part = find_partition();
    if (part == nullptr) {
        ESP_LOGI(kTag, "no \"%s\" partition (old partition table?) — sanoTTS unavailable", kPart);
        return false;
    }
    // 重みはパーティション全体 (1 MB) を貼る。blob に総サイズは無く、コアがテンソルの
    // オフセットをこの size で境界検査する。未書き込み (0xFF) なら magic で弾かれる。
    const void* p = nullptr;
    esp_partition_mmap_handle_t mh = 0;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &p, &mh) != ESP_OK) {
        ESP_LOGW(kTag, "mmap failed (%u B)", static_cast<unsigned>(part->size));
        return false;
    }
    if (!jtts::set_sano_model({static_cast<const std::uint8_t*>(p), part->size})) {
        ESP_LOGW(kTag, "no valid model in \"%s\" partition — flash assets/sanotts/saanotts-jp-v3-int8.bin "
                       "(make flash writes it)", kPart);
        esp_partition_munmap(mh);
        return false;
    }
    g_mmap = mh;
    ESP_LOGI(kTag, "sanoTTS model mapped from \"%s\" @0x%x", kPart, static_cast<unsigned>(part->address));
#if defined(CONFIG_JTTS_SANO_KANJI)
    // 端末内漢字 G2P: dict パーティション (13.7 MB) を esp_mmu_map で貼って登録する。
    // 開けなくても sanoTTS 自体はかな入力で使える。
    if (saan_dict_open(&g_dict) && jtts::set_sano_dict(&g_dict)) {
        g_dict_ok = true;
        ESP_LOGI(kTag, "kanji G2P dictionary active (%u surfaces / %u entries) — kanji text accepted",
                 static_cast<unsigned>(g_dict.n_surfaces), static_cast<unsigned>(g_dict.n_entries));
    } else {
        ESP_LOGW(kTag, "kanji dictionary unavailable — flash assets/sanotts/k1-dict-438750.bin to the "
                       "\"dict\" partition (make flash BOARD=cores3-dict). Kana-only input.");
    }
#endif
    return true;
#endif
}

Status status() {
    Status st;
    st.loaded = jtts::sano_model_loaded();
    st.dict = jtts::sano_dict_loaded();
    if (const esp_partition_t* part = find_partition(); part != nullptr) {
        st.capacity = part->size;
    }
    return st;
}

}  // namespace stackchan::app::sano_model

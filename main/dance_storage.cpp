// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "dance_storage.hpp"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "dance.hpp"

namespace stackchan::app::dance_storage {

namespace {
constexpr const char* kTag = "dance-store";
constexpr const char* kPart = "storage";  // voice_db / avatar_vm と同じ NVS パーティション
constexpr const char* kNs = "dance";
constexpr const char* kKey = "song0";
// 上限: 1MB storage を voice_db 等と分け合うので、テスト用途で控えめに。
constexpr std::size_t kMaxBlob = 400 * 1024;

// "storage" パーティションの NVS を初期化する (avatar_vm/voice_db が先に呼ぶ場合
// もあるので二重初期化を許容)。
bool ensure_partition() {
    const esp_partition_t* p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPart);
    if (p == nullptr) {
        ESP_LOGW(kTag, "no \"%s\" partition", kPart);
        return false;
    }
    esp_err_t err = nvs_flash_init_partition(kPart);  // idempotent; 別モジュール init 済みでも OK
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(kPart);
        err = nvs_flash_init_partition(kPart);
    }
    return err == ESP_OK;
}

// blob の最小妥当性 (magic / version / サイズ整合)。
bool valid_blob(const std::uint8_t* d, std::size_t len) {
    if (len < sizeof(DanceBlobHeader)) return false;
    DanceBlobHeader h;
    std::memcpy(&h, d, sizeof(h));
    if (h.magic != kDanceMagic || h.version != 1) return false;
    const std::size_t need = static_cast<std::size_t>(h.audio_offset) + h.audio_len;
    if (h.audio_offset != sizeof(DanceBlobHeader) + static_cast<std::size_t>(h.kf_count) * 12) {
        return false;
    }
    return need <= len;
}
}  // namespace

bool save(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0 || len > kMaxBlob) {
        ESP_LOGW(kTag, "reject blob (len=%u)", static_cast<unsigned>(len));
        return false;
    }
    if (!valid_blob(data, len)) {
        ESP_LOGW(kTag, "reject blob (bad header / size mismatch)");
        return false;
    }
    ensure_partition();
    nvs_handle_t h;
    if (nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open failed");
        return false;
    }
    esp_err_t err = nvs_set_blob(h, kKey, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_set_blob failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "saved dance blob (%u bytes)", static_cast<unsigned>(len));
    return true;
}

std::vector<std::uint8_t> load() {
    ensure_partition();
    nvs_handle_t h;
    if (nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h) != ESP_OK) return {};
    std::size_t sz = 0;
    esp_err_t err = nvs_get_blob(h, kKey, nullptr, &sz);
    if (err != ESP_OK || sz == 0 || sz > kMaxBlob) {
        nvs_close(h);
        return {};
    }
    std::vector<std::uint8_t> buf(sz);
    err = nvs_get_blob(h, kKey, buf.data(), &sz);
    nvs_close(h);
    if (err != ESP_OK) return {};
    if (!valid_blob(buf.data(), buf.size())) {
        ESP_LOGW(kTag, "stored blob invalid — ignoring");
        return {};
    }
    ESP_LOGI(kTag, "loaded dance blob (%u bytes)", static_cast<unsigned>(sz));
    return buf;
}

}  // namespace stackchan::app::dance_storage

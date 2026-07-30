// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// [Dance P0] ダンス機能の go/no-go: サーボとスピーカーが同一電源レールを共有し、
// 移動時の突入電流で音声が glitch する懸念 (docs/dance-feature-research.md §2) を
// 実機で確かめる。servo.dance_active を立てて masking を無効化し、melody を鳴らし
// ながらサーボを踊らせる。ユーザーが音を聴いて劣化の許容度を判定する。

#include "dance_poc.hpp"

#include "sdkconfig.h"

#if defined(CONFIG_STACKCHAN_DANCE_POC)

#include <cstdint>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>

#include "shared_state.hpp"

namespace {
constexpr const char* kTag = "dance-poc";

// 確定した座標仕様: 符号付き ±100% (0=キャリブ中心, +100=軸max, -100=軸min)。
float deg_from_pct(float pct, int min_deg, int max_deg) {
    if (pct >= 0.0f) return pct * 0.01f * static_cast<float>(max_deg);
    return pct * 0.01f * static_cast<float>(-min_deg);  // pct<0, -min_deg>0 → 負
}

struct Beat {
    float freq;       // 音の高さ [Hz] (0 = 休符)
    std::uint32_t ms; // このビートの長さ
    float yaw_pct;    // このビートで向かう位置 (±100%)
    float pitch_pct;
    std::uint16_t speed; // servo goal speed (大きいほど速い=突入電流大)
};

// ペンタトニックの短いフレーズ + ダンス動作。速い大振幅の move (突入電流最大) と
// 音を意図的に重ねて最悪ケースを作る。
constexpr Beat kSeq[] = {
    {523.25f, 300, +60, +40, 900},  // C5  速い大 yaw ふり
    {659.25f, 300, -60, -60, 900},  // E5  逆へ速い大ふり
    {783.99f, 450, +40, +80, 500},  // G5  ゆっくり pitch 上げ (発音中に移動)
    {659.25f, 300, -40, +20, 900},
    {587.33f, 300, +80, -40, 900},  // D5  最大 yaw
    {523.25f, 600, 0, 0, 300},      // C5  センターへゆっくり (静音移動の対照)
    {0.0f, 250, +30, +60, 700},     // 休符中に移動 (音無しでの動作音の確認)
    {440.00f, 400, -30, -20, 900},  // A4
};

struct DanceArgs {
    stackchan::app::SharedState* st;
    int yaw_min, yaw_max, pitch_min, pitch_max;
};

void dance_task(void* arg) {
    auto* a = static_cast<DanceArgs*>(arg);
    auto* st = a->st;

    M5.Speaker.setVolume(180);
    st->servo.enabled.store(true, std::memory_order_relaxed);
    st->servo.dance_active.store(true, std::memory_order_relaxed);
    ESP_LOGI(kTag, "DANCE P0 start — melody + servo sweep (masking bypassed). listen for audio glitches.");

    constexpr int kLoops = 4;  // 全体で ~12s 程度
    for (int loop = 0; loop < kLoops; ++loop) {
        for (const auto& b : kSeq) {
            const float yaw = deg_from_pct(b.yaw_pct, a->yaw_min, a->yaw_max);
            const float pitch = deg_from_pct(b.pitch_pct, a->pitch_min, a->pitch_max);
            st->servo.speed_override.store(b.speed, std::memory_order_relaxed);
            st->servo.target_yaw_deg.store(yaw, std::memory_order_relaxed);
            st->servo.target_pitch_deg.store(pitch, std::memory_order_relaxed);
            if (b.freq > 0.0f) {
                M5.Speaker.tone(b.freq, b.ms);  // 非ブロッキング; 移動と同時に鳴る
            }
            vTaskDelay(pdMS_TO_TICKS(b.ms));
        }
    }

    // 後始末: センターへ戻し、通常挙動へ復帰。
    st->servo.speed_override.store(0, std::memory_order_relaxed);
    st->servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
    st->servo.target_pitch_deg.store(0.0f, std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(800));
    st->servo.dance_active.store(false, std::memory_order_relaxed);
    ESP_LOGI(kTag, "DANCE P0 done.");
    delete a;
    vTaskDelete(nullptr);
}
}  // namespace

namespace stackchan::app {

void dance_poc_run(SharedState& state, const ServoLimits& limits) {
    auto* a = new DanceArgs{&state, limits.yaw_min_deg, limits.yaw_max_deg,
                            limits.pitch_min_deg, limits.pitch_max_deg};
    xTaskCreatePinnedToCore(dance_task, "dance_poc", 4096, a, 5, nullptr, 1);
}

}  // namespace stackchan::app

#else  // !CONFIG_STACKCHAN_DANCE_POC

#include "shared_state.hpp"
namespace stackchan::app {
void dance_poc_run(SharedState&, const ServoLimits&) {}
}  // namespace stackchan::app

#endif  // CONFIG_STACKCHAN_DANCE_POC

// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "dance.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>

#include "dance_storage.hpp"
#include "shared_state.hpp"

namespace stackchan::app {

namespace {
constexpr const char* kTag = "dance";
constexpr std::uint32_t kSampleRate = 16000;

SharedState* g_state = nullptr;
ServoLimits g_limits{};

// 符号付き ±100% → 度 (0=中心, +100=軸max, -100=軸min)。servo_task 側でも limits
// クランプが効く。
float yaw_deg(std::int16_t pct_x100) {
    const float pct = pct_x100 * 0.01f;
    return pct >= 0 ? pct * 0.01f * g_limits.yaw_max_deg : pct * 0.01f * (-g_limits.yaw_min_deg);
}
float pitch_deg(std::int16_t pct_x100) {
    const float pct = pct_x100 * 0.01f;
    return pct >= 0 ? pct * 0.01f * g_limits.pitch_max_deg : pct * 0.01f * (-g_limits.pitch_min_deg);
}

// --- 埋め込みサンプル "曲" (P2: 生成 PCM メロディ + 同期キーフレーム) ----------
// P3 でフラッシュ上の (AAC 音声 + キーフレーム blob) に置き換える。ここでは 1 ステップ
// = 1 拍として、音 (freq/長さ) と 頭の目標姿勢 (yaw/pitch %, 移動時間) を一体で持つ。
struct Step {
    float freq;          // 音の高さ [Hz] (0 = 休符)
    std::uint16_t ms;    // 拍の長さ
    std::int16_t yaw;    // 目標 yaw%  (整数%)
    std::int16_t pitch;  // 目標 pitch% (整数%)
    std::uint16_t move;  // 移動にかける時間 [ms]
};

// ペンタトニックの軽快なループ + 頭のノリ (左右+上下)。~13 秒。
constexpr Step kSong[] = {
    {523, 380, +60, +30, 260}, {587, 380, -60, +30, 260}, {659, 380, +50, -30, 260},
    {587, 380, -50, -30, 260}, {523, 380, +70, +50, 220}, {659, 380, -70, +50, 220},
    {783, 500, 0, +90, 480},   {659, 260, -40, +30, 200}, {587, 380, +40, -20, 240},
    {523, 380, -40, -20, 240}, {440, 380, +80, +40, 220}, {523, 380, -80, +40, 220},
    {587, 500, 0, 0, 460},     {0, 240, +30, +70, 200},   {659, 380, -30, +20, 240},
    {783, 380, +55, -30, 240}, {880, 500, 0, +100, 480},  {783, 260, -55, +40, 200},
    {659, 380, +65, -40, 220}, {587, 380, -65, -40, 220}, {523, 620, 0, 0, 560},
    {0, 300, 0, +40, 260},     {523, 300, +90, +30, 180}, {587, 300, -90, +30, 180},
    {659, 300, +90, -30, 180}, {783, 600, 0, +60, 520},   {523, 700, 0, 0, 640},
};
constexpr int kSteps = static_cast<int>(sizeof(kSong) / sizeof(kSong[0]));

// 生成した PCM (PSRAM, 全曲) と 展開したキーフレーム。初回に一度だけ構築。
std::int16_t* g_pcm = nullptr;
std::size_t g_pcm_len = 0;
std::vector<DanceKeyframe> g_keyframes;
std::uint32_t g_total_ms = 0;

// メロディを 16kHz mono int16 PCM に描画する (簡易エンベロープでクリック回避)。
void build_song() {
    if (g_pcm != nullptr) return;
    // 総サンプル数を数える。
    std::size_t total = 0;
    for (const auto& s : kSong) total += static_cast<std::size_t>(s.ms) * kSampleRate / 1000;
    g_pcm = static_cast<std::int16_t*>(heap_caps_malloc(total * sizeof(std::int16_t), MALLOC_CAP_SPIRAM));
    if (g_pcm == nullptr) {
        ESP_LOGE(kTag, "PCM alloc failed (%u samples)", static_cast<unsigned>(total));
        return;
    }
    g_pcm_len = total;

    std::size_t pos = 0;
    std::uint32_t t = 0;
    g_keyframes.clear();
    for (const auto& s : kSong) {
        const std::size_t n = static_cast<std::size_t>(s.ms) * kSampleRate / 1000;
        const std::size_t atk = kSampleRate * 5 / 1000;   // 5ms attack
        const std::size_t rel = kSampleRate * 12 / 1000;  // 12ms release
        for (std::size_t i = 0; i < n; ++i) {
            float v = 0.0f;
            if (s.freq > 0.0f) {
                const float ph = 2.0f * static_cast<float>(M_PI) * s.freq * i / kSampleRate;
                v = std::sin(ph) + 0.30f * std::sin(3.0f * ph);  // 基音 + 3倍音で音色
                float env = 1.0f;
                if (i < atk) env = static_cast<float>(i) / atk;
                else if (i > n - rel) env = static_cast<float>(n - i) / rel;
                v *= env;
            }
            int sample = static_cast<int>(v * 6000.0f);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            g_pcm[pos++] = static_cast<std::int16_t>(sample);
        }
        // このステップ = 1 キーフレーム。
        g_keyframes.push_back(DanceKeyframe{t, s.move, static_cast<std::int16_t>(s.yaw * 100),
                                            static_cast<std::int16_t>(s.pitch * 100), 0});
        t += s.ms;
    }
    g_total_ms = t;
    ESP_LOGI(kTag, "song built: %d steps, %u ms, %u PCM samples (PSRAM)", kSteps,
             static_cast<unsigned>(g_total_ms), static_cast<unsigned>(g_pcm_len));
}

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

// 1 回のダンスを実行する (ブロッキング; エンジン タスク内で呼ぶ)。フラッシュに
// アップロード済み blob があればそれを、無ければ埋め込みサンプルを再生する。
void run_dance() {
    // キーフレーム / 音声の実体を指すビュー。blob 経由 or 埋め込み。
    const DanceKeyframe* kfs = nullptr;
    std::size_t kf_n = 0;
    const std::int16_t* pcm = nullptr;
    std::size_t pcm_n = 0;
    std::uint32_t rate = kSampleRate;
    std::uint32_t total_ms = 0;

    std::vector<std::uint8_t> blob = dance_storage::load();  // dance 中 生存させる
    if (blob.size() >= sizeof(DanceBlobHeader)) {
        DanceBlobHeader h;
        std::memcpy(&h, blob.data(), sizeof(h));
        if (h.audio_fmt == 0 /* PCM16 */ && h.kf_count > 0) {
            kfs = reinterpret_cast<const DanceKeyframe*>(blob.data() + sizeof(DanceBlobHeader));
            kf_n = h.kf_count;
            pcm = reinterpret_cast<const std::int16_t*>(blob.data() + h.audio_offset);
            pcm_n = h.audio_len / sizeof(std::int16_t);
            rate = h.sample_rate;
            total_ms = h.total_ms;
            ESP_LOGI(kTag, "using stored blob: %u kf, %u ms, %u Hz, %u samples", kf_n,
                     static_cast<unsigned>(total_ms), static_cast<unsigned>(rate),
                     static_cast<unsigned>(pcm_n));
        } else {
            ESP_LOGW(kTag, "stored blob audio_fmt=%u unsupported — using embedded", h.audio_fmt);
        }
    }
    if (kfs == nullptr) {  // フォールバック: 埋め込み生成メロディ
        build_song();
        if (g_pcm == nullptr || g_keyframes.empty()) return;
        kfs = g_keyframes.data();
        kf_n = g_keyframes.size();
        pcm = g_pcm;
        pcm_n = g_pcm_len;
        rate = kSampleRate;
        total_ms = g_total_ms;
    }

    ESP_LOGI(kTag, "dance start");
    g_state->dance.active.store(true, std::memory_order_relaxed);
    g_state->servo.dance_active.store(true, std::memory_order_relaxed);  // masking バイパス
    g_state->servo.enabled.store(true, std::memory_order_relaxed);
    // マスター音量には触らない: speaker-volume 設定 (ミュート含む) をそのまま
    // 使う。P0 実験の固定 setVolume(180) は「dance 後も 180 のまま残る」
    // 「ミュート中でも鳴る」ため撤去 (電源レール耐性は 180 で確認済みなので
    // それ以下の設定音量はすべて安全側)。
    M5.Speaker.playRaw(pcm, pcm_n, rate, false);  // 非ブロッキング
    const std::uint32_t t0 = now_ms();

    std::size_t next = 0;
    for (;;) {
        // stop コマンド (2) で中断。
        if (g_state->dance.command.load(std::memory_order_relaxed) == 2) {
            g_state->dance.command.store(0, std::memory_order_relaxed);
            ESP_LOGI(kTag, "dance stop (requested)");
            break;
        }
        const std::uint32_t elapsed = now_ms() - t0;
        g_state->dance.elapsed_ms.store(elapsed, std::memory_order_relaxed);

        // 期限が来たキーフレームを順に適用 (取りこぼしても最新へ追従)。
        while (next < kf_n && kfs[next].time_ms <= elapsed) {
            const DanceKeyframe& kf = kfs[next];
            g_state->servo.move_time_ms.store(kf.move_ms, std::memory_order_relaxed);
            g_state->servo.target_yaw_deg.store(yaw_deg(kf.yaw_pct_x100), std::memory_order_relaxed);
            g_state->servo.target_pitch_deg.store(pitch_deg(kf.pitch_pct_x100),
                                                  std::memory_order_relaxed);
            ++next;
        }
        // 全キーフレーム消化 + 音声終了で完了。
        if (next >= kf_n && elapsed >= total_ms && !M5.Speaker.isPlaying()) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 後始末: 音声停止・センターへ戻し・通常挙動へ復帰。
    M5.Speaker.stop();
    g_state->servo.move_time_ms.store(600, std::memory_order_relaxed);
    g_state->servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
    g_state->servo.target_pitch_deg.store(0.0f, std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(700));
    g_state->servo.move_time_ms.store(0, std::memory_order_relaxed);
    g_state->servo.speed_override.store(0, std::memory_order_relaxed);
    g_state->servo.dance_active.store(false, std::memory_order_relaxed);
    g_state->dance.active.store(false, std::memory_order_relaxed);
    g_state->dance.elapsed_ms.store(0, std::memory_order_relaxed);
    ESP_LOGI(kTag, "dance done");
}

void engine_task(void*) {
    for (;;) {
        if (g_state->dance.command.load(std::memory_order_relaxed) == 1) {
            g_state->dance.command.store(0, std::memory_order_relaxed);
            run_dance();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}  // namespace

void start_dance_engine(SharedState& state, const ServoLimits& limits) {
    g_state = &state;
    g_limits = limits;
    xTaskCreatePinnedToCore(engine_task, "dance", 4096, nullptr, 4, nullptr, 1);
    ESP_LOGI(kTag, "dance engine ready");
}

void dance_control(std::uint8_t cmd, std::uint8_t id) {
    if (g_state == nullptr) return;
    if (cmd == 1) {
        g_state->dance.select.store(id, std::memory_order_relaxed);
        g_state->dance.command.store(1, std::memory_order_relaxed);
    } else if (cmd == 2) {
        g_state->dance.command.store(2, std::memory_order_relaxed);
    }
}

bool dance_upload(const std::uint8_t* data, std::size_t len) {
    return dance_storage::save(data, len);
}

}  // namespace stackchan::app

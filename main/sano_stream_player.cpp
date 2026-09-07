// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "sano_stream_player.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "shared_state.hpp"
#include "speech.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "sano-stream";

constexpr int kSpkChannel = 0;              // M5.Speaker の仮想チャンネル (isPlaying(ch) で空きを見る)
constexpr std::size_t kChunk = jtts::SanoStream::kChunkSamples;  // 2,048 sample ≈ 93 ms
constexpr std::uint32_t kEnvStepMs = Speech::kEnvelopeStepMs;   // 16 ms 窓
constexpr std::uint32_t kLipPeriodMs = 10;
// playRaw してから実際に鳴るまでの遅れ (DMA バッファぶん) の見込み。上流と同じ仮置き。
constexpr std::int64_t kLipLatencyUs = 45'000;
// M5.Speaker が再生位置より先に読む量 (DMA 先読み)。追い越し検出に使う。
constexpr std::size_t kReadahead = 2048;

// 先読み量の見込みに使う xRT (合成時間 / 音声時間)。前回の実測で更新する。
// 初期値は CoreS3 + PSRAM arena の実測 (2026-09-07: 2.0〜2.3) に余裕を持たせたもの。
constexpr float kXrtInitial = 2.5f;
constexpr float kXrtMargin = 1.15f;
float g_xrt_est = kXrtInitial;

std::size_t preroll_target(std::size_t total) {
    const float x = g_xrt_est;
    const float ratio = x > 1.0f ? 1.0f - 1.0f / x : 0.0f;
    std::size_t p = static_cast<std::size_t>(static_cast<float>(total) * ratio) + 2 * kChunk;
    return std::min(p, total);
}

// 発話 1 本ぶんの共有状態 (合成側が書き、lip task が読む)。
struct Utterance {
    std::int16_t* buf = nullptr;      // PSRAM、総サンプル数ぶん (ゼロ埋め)
    std::size_t total = 0;
    std::uint32_t rate = 22050;
    std::size_t window = 0;           // 包絡 1 窓のサンプル数
    std::vector<float> env;           // 窓ごとのピーク (0..1、未正規化)
    std::atomic<std::size_t> fill{0}; // 変換済みサンプル数
    std::atomic<float> env_max{0.0f};
    std::atomic<std::int64_t> play_t0_us{0};
    std::atomic<std::size_t> play_base{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> lip_quit{false};
    std::atomic<bool> lip_done{false};
    SharedState* state = nullptr;
};

// 再生位置 (sample) の推定。鳴っていなければ npos。
std::size_t play_position(const Utterance& u) {
    if (!u.playing.load(std::memory_order_acquire)) return SIZE_MAX;
    if (M5.Speaker.isPlaying(kSpkChannel) == 0) return SIZE_MAX;
    const std::int64_t dt = esp_timer_get_time() - u.play_t0_us.load(std::memory_order_relaxed) - kLipLatencyUs;
    if (dt < 0) return SIZE_MAX;
    return u.play_base.load(std::memory_order_relaxed) +
           static_cast<std::size_t>(dt * static_cast<std::int64_t>(u.rate) / 1'000'000);
}

// 口の開き (0..1)。再生位置の窓の包絡をここまでの最大で正規化する。
float mouth_open_now(const Utterance& u) {
    const std::size_t pos = play_position(u);
    if (pos == SIZE_MAX) return 0.0f;
    const std::size_t fill = u.fill.load(std::memory_order_acquire);
    if (pos >= fill || u.window == 0) return 0.0f;   // まだ変換していない / 途切れ
    const std::size_t idx = pos / u.window;
    if (idx >= u.env.size()) return 0.0f;
    float e = u.env[idx];
    // 隣の窓と線形補間 (段差を無くす)。次の窓は変換が済んでいるときだけ使う
    if (idx + 1 < u.env.size() && (idx + 2) * u.window <= fill) {
        const float t = static_cast<float>(pos - idx * u.window) / static_cast<float>(u.window);
        e += t * (u.env[idx + 1] - e);
    }
    float m = u.env_max.load(std::memory_order_relaxed);
    if (m < 0.03f) m = 0.03f;                // 無音に近い発話で 0/0 を作らない
    if (e < 0.02f) return 0.0f;              // 床 (息の音で口が震えない)
    return std::min(e / m, 1.0f);
}

void lip_task(void* arg) {
    auto* u = static_cast<Utterance*>(arg);
    while (!u->lip_quit.load(std::memory_order_acquire)) {
        if (u->state != nullptr) {
            u->state->face.mouth_open.store(mouth_open_now(*u), std::memory_order_relaxed);
        }
        vTaskDelay(pdMS_TO_TICKS(kLipPeriodMs));
    }
    if (u->state != nullptr) u->state->face.mouth_open.store(0.0f, std::memory_order_relaxed);
    u->lip_done.store(true, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

// 変換済みチャンクを発話バッファに追記し、包絡を更新する。
void append_chunk(Utterance& u, const std::int16_t* pcm, std::size_t n) {
    const std::size_t base = u.fill.load(std::memory_order_relaxed);
    std::memcpy(u.buf + base, pcm, n * sizeof(std::int16_t));
    float env_max = u.env_max.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = (base + i) / u.window;
        if (idx >= u.env.size()) break;
        const float a = static_cast<float>(std::abs(static_cast<int>(pcm[i]))) / 32767.0f;
        if (a > u.env[idx]) u.env[idx] = a;
        if (a > env_max) env_max = a;
    }
    u.env_max.store(env_max, std::memory_order_relaxed);
    u.fill.store(base + n, std::memory_order_release);
}

// 貯めたぶん + 残り全部 (ゼロ埋め) の 2 区間を渡して鳴らし始める。
bool start_playback(Utterance& u) {
    const std::size_t fill = u.fill.load(std::memory_order_acquire);
    u.play_base.store(0, std::memory_order_relaxed);
    u.play_t0_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    u.playing.store(true, std::memory_order_release);
    if (!M5.Speaker.playRaw(u.buf, fill, u.rate, /*stereo=*/false, /*repeat=*/1, kSpkChannel,
                            /*stop_current=*/false)) {
        ESP_LOGE(kTag, "playRaw (preroll %u) failed", static_cast<unsigned>(fill));
        return false;
    }
    if (fill < u.total &&
        !M5.Speaker.playRaw(u.buf + fill, u.total - fill, u.rate, false, 1, kSpkChannel, false)) {
        ESP_LOGE(kTag, "playRaw (rest %u) failed", static_cast<unsigned>(u.total - fill));
        return false;
    }
    return true;
}

// 再生 (DMA 先読み込み) が書き込みを追い越したか。
bool overrun(const Utterance& u) {
    const std::size_t pos = play_position(u);
    if (pos == SIZE_MAX) return false;
    return pos + kReadahead + static_cast<std::size_t>(kLipLatencyUs * u.rate / 1'000'000) >
           u.fill.load(std::memory_order_acquire);
}

} // namespace

bool speak_streaming(std::u32string_view kana, const jtts::Options& opt, SharedState* state) {
    jtts::SanoStream stream;
    if (!stream.begin(kana, opt)) return false;

    Utterance u;
    u.total = stream.total_samples();
    u.rate = stream.sample_rate();
    u.state = state;
    u.window = static_cast<std::size_t>(u.rate) * kEnvStepMs / 1000u;
    u.buf = static_cast<std::int16_t*>(
        heap_caps_calloc(u.total, sizeof(std::int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (u.buf == nullptr) {
        ESP_LOGE(kTag, "utterance buffer (%u samples) alloc failed", static_cast<unsigned>(u.total));
        return false;
    }
    u.env.assign((u.total + u.window - 1) / u.window, 0.0f);

    // lip task (core 1、PSRAM スタック)。合成タスクと同じ core に置くと合成に潰される。
    TaskHandle_t lip = nullptr;
    xTaskCreatePinnedToCoreWithCaps(lip_task, "sano_lip", 3 * 1024, &u, tskIDLE_PRIORITY + 3, &lip, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lip == nullptr) {
        ESP_LOGW(kTag, "lip task create failed — no lip sync for this utterance");
        u.lip_done.store(true);
    }

    const std::size_t target = preroll_target(u.total);
    ESP_LOGI(kTag, "preroll %u / %u samples (%.0f%%) — xRT est %.2f", static_cast<unsigned>(target),
             static_cast<unsigned>(u.total), 100.0 * static_cast<double>(target) / static_cast<double>(u.total),
             static_cast<double>(g_xrt_est));

    // 前の音が鳴っていたら待つ (このチャンネルは自分だけが使う)
    while (M5.Speaker.isPlaying(kSpkChannel) != 0) vTaskDelay(pdMS_TO_TICKS(10));

    std::vector<std::int16_t> chunk(kChunk);
    const std::int64_t t_begin = esp_timer_get_time();
    std::int64_t t_first = 0, t_rest = 0;
    int chunks = 0, gaps = 0;
    bool started = false, ok = true;
    double t_ready_ms = 0.0;
    for (;;) {
        const std::int64_t t0 = esp_timer_get_time();
        const int n = stream.pull(chunk.data(), chunk.size());
        const std::int64_t dt = esp_timer_get_time() - t0;
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (chunks == 0) t_first = dt; else t_rest += dt;
        ++chunks;
        if (u.fill.load() + static_cast<std::size_t>(n) > u.total) {
            ESP_LOGE(kTag, "pull exceeded the utterance size");
            ok = false;
            break;
        }
        append_chunk(u, chunk.data(), static_cast<std::size_t>(n));
        if (!started) {
            if (u.fill.load() >= target) {
                t_ready_ms = static_cast<double>(esp_timer_get_time() - t_begin) / 1000.0;
                if (!start_playback(u)) { ok = false; break; }
                started = true;
            }
        } else if (overrun(u)) {
            ++gaps;
        }
        vTaskDelay(1);  // idle / 同優先度タスクに回す (WDT)
    }
    stream.end();  // arena とモデルのロックを早めに返す
    if (ok && !started) {
        t_ready_ms = static_cast<double>(esp_timer_get_time() - t_begin) / 1000.0;
        if (!start_playback(u)) ok = false;
        started = true;
    }

    // 鳴り終わるまで待つ (playRaw はポインタを持つだけなので、解放はその後)
    while (M5.Speaker.isPlaying(kSpkChannel) != 0) vTaskDelay(pdMS_TO_TICKS(10));
    u.playing.store(false, std::memory_order_release);
    u.lip_quit.store(true, std::memory_order_release);
    while (!u.lip_done.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(5));
    heap_caps_free(u.buf);
    u.buf = nullptr;

    const double audio_ms = 1000.0 * static_cast<double>(u.total) / u.rate;
    const double chunk_ms = 1000.0 * static_cast<double>(kChunk) / u.rate;
    const double mean_rest_ms = chunks > 1 ? static_cast<double>(t_rest) / (chunks - 1) / 1000.0 : 0.0;
    const double xrt = chunk_ms > 0 ? mean_rest_ms / chunk_ms : 0.0;
    ESP_LOGI(kTag, "%d chunks / audio %.0f ms: first pull %.0f ms, steady xRT %.2f, "
                   "speech started after %.0f ms, %d overrun(s)",
             chunks, audio_ms, static_cast<double>(t_first) / 1000.0, xrt, t_ready_ms, gaps);
    if (chunks > 2 && xrt > 0.0) {
        float est = static_cast<float>(xrt) * kXrtMargin * (gaps > 0 ? 1.2f : 1.0f);
        g_xrt_est = std::clamp(est, 0.3f, 10.0f);
    }
    if (gaps > 0) {
        ESP_LOGW(kTag, "playback overran synthesis — next utterance prerolls with xRT %.2f",
                 static_cast<double>(g_xrt_est));
    }
    return ok;
}

} // namespace stackchan::app

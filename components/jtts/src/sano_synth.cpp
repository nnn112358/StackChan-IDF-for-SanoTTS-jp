// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS-jp (蒸留ニューラル TTS、559 K params) エンジン。components/saanotts_core の
// C99 推論コアを jtts::Engine::Sano として使う。
//
//   かな (jtts 表記) ──▶ かな中間表現 (sano_text.cpp) ──▶ saan_g2p ──▶ ids
//     ──▶ saan_stream_init / pull (22.05 kHz float) ──▶ ピーク正規化
//     ──▶ リサンプル (resampler.cpp) ──▶ int16 @ opt.sample_rate_hz
//
// 発話は**全部合成してから**返す (Speech::say がそのまま playRaw + 包絡を作るので、
// リップシンクは他エンジンと同じ経路)。上流 (SanoTTS-jp-M5StackCoreS3) のストリーミング
// 再生はこの firmware の一括再生の Speech には合わせない。
//
// メモリ: 作業領域 (arena) 176 KB を発話ごとに取って返す。既定は PSRAM
// (CONFIG_JTTS_SANO_ARENA_INTERNAL で内部 DRAM を先に試す)。重み blob は flash mmap
// のまま読む (コピーしない)。
//
// CONFIG_JTTS_ENABLE_SANO 無効ボードではスタブになり、推論コアはリンクされない。
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"

#if !defined(ESP_PLATFORM) || defined(CONFIG_JTTS_ENABLE_SANO)
#define JTTS_SANO_AVAILABLE 1
#endif

#ifdef JTTS_SANO_AVAILABLE

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include "resampler.hpp"

extern "C" {
#include "g2p.h"
#include "saanotts.h"
#include "saanotts_stream.h"
}

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define SANO_LOGI(...) ESP_LOGI("jtts-sano", __VA_ARGS__)
#define SANO_LOGW(...) ESP_LOGW("jtts-sano", __VA_ARGS__)
#else
#include <cstdio>
#define SANO_LOGI(...)                            \
    do {                                          \
        std::fprintf(stderr, "[jtts-sano] ");     \
        std::fprintf(stderr, __VA_ARGS__);        \
        std::fprintf(stderr, "\n");               \
    } while (0)
#define SANO_LOGW SANO_LOGI
#endif

namespace stackchan::jtts {

namespace {

// ---- 定数 -----------------------------------------------------------------

// 受け付ける ids の上限。arena は 520 ids まで持つが、生徒が学習したのは 350 ids 相当
// までで、その外は分布外 (上流 main.c の SAAN_MAX_IDS)。長い文は拒否してフォールバック。
constexpr std::int32_t kMaxIds = 350;

// 作業領域。上流実測 (T4 後): n_ids=350 で init/pull が通る最小 160,768 B、176 KB 固定で
// 450 ids まで通る。PIE のため 16 バイト境界。
constexpr std::size_t kArenaBytes = 176 * 1024;
constexpr std::size_t kArenaAlign = 16;

// 発話長の安全弁 (sample @22.05 kHz)。実際の日本語は 1 ids あたり数 frame。
constexpr std::size_t kMaxSamples = static_cast<std::size_t>(SAAN_SR) * 30u;

// mora_ms (既定 110 ms = 等速) → duration scale の倍率。HMM エンジンと同じ範囲。
constexpr float kBaseMoraMs = 110.0f;
constexpr float kTempoMin = 0.5f, kTempoMax = 2.0f;

// 音量正規化: 発話のピークを kPeakTarget に合わせる (opt.gain は既定 0.6 に対する
// 相対倍率として掛ける)。目標 0.3 は上流 SanoTTS-jp-M5StackCoreS3 の実測レベル
// (デモ文で |max| 9627/32767 ≈ 0.29、正規化無し) に合わせたもので、CoreS3 の内蔵
// スピーカー + ブースト無効の AW88298 ではこのあたりが歪まず聞き取れる上限だった
// (2026-09-07: 0.9 では音量 100% でも割れて聞き取りづらかった)。大きくしたいときは
// 音量 % (M5 側のデジタル ゲイン、100% = 等倍) か jtts 設定の gain を上げる。
// 無音に近い出力を増幅しすぎないよう倍率は 4 倍まで。
constexpr float kPeakTarget = 0.3f;
constexpr float kDefaultGain = 0.6f;
constexpr float kPeakCeiling = 0.98f;
constexpr float kMaxGainBoost = 4.0f;
constexpr float kSilencePeak = 1e-4f;

// ---- モデル ---------------------------------------------------------------

struct Model {
    saan_weights weights{};
    bool loaded = false;
    // 重みの差し替え (将来の HTTP アップロード) と合成を直列化する。合成は数秒保持する。
    std::mutex mutex;
};
Model g_model;

// ---- arena ----------------------------------------------------------------

struct ArenaDeleter {
    void operator()(void* p) const noexcept {
#if defined(ESP_PLATFORM)
        heap_caps_free(p);
#else
        std::free(p);
#endif
    }
};
using ArenaPtr = std::unique_ptr<std::uint8_t, ArenaDeleter>;

ArenaPtr alloc_arena() {
#if defined(ESP_PLATFORM)
#if defined(CONFIG_JTTS_SANO_ARENA_INTERNAL)
    constexpr std::uint32_t kFirst = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr std::uint32_t kSecond = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    constexpr std::uint32_t kFirst = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    constexpr std::uint32_t kSecond = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    auto name = [](std::uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : "internal DRAM"; };
    void* p = heap_caps_aligned_alloc(kArenaAlign, kArenaBytes, kFirst);
    const char* where = name(kFirst);
    if (p == nullptr) {
        p = heap_caps_aligned_alloc(kArenaAlign, kArenaBytes, kSecond);
        where = name(kSecond);
    }
    static bool s_logged = false;
    if (p != nullptr && !s_logged) {
        s_logged = true;
        SANO_LOGI("arena %u B in %s", static_cast<unsigned>(kArenaBytes), where);
    }
    return ArenaPtr{static_cast<std::uint8_t*>(p)};
#else
    return ArenaPtr{static_cast<std::uint8_t*>(std::aligned_alloc(kArenaAlign, kArenaBytes))};
#endif
}

// ---- 段階ごとの処理 ---------------------------------------------------------

// 読み → 音素 ID 列。読めない / 空 / 長すぎるは false。
bool text_to_ids(std::u32string_view text, std::vector<std::int32_t>& ids) {
    std::string inter;
    std::size_t skipped = 0;
    if (!internal::build_sano_intermediate(text, inter, &skipped)) return false;
    if (skipped > 0) {
        SANO_LOGW("reading: %u unpronounceable code point(s) skipped", static_cast<unsigned>(skipped));
    }

    const std::int32_t cap = saan_g2p_capacity(inter.size());  // 上流の式 (2 × バイト数 + 3)
    ids.assign(static_cast<std::size_t>(cap), 0);
    std::int32_t n_ids = 0;
    saan_g2p_info info{};
    const saan_g2p_status s = saan_g2p(inter.data(), inter.size(), ids.data(), cap, &n_ids, &info);
    if (s != SAAN_G2P_OK) {
        SANO_LOGW("g2p failed: %s (byte %d) in \"%s\"", saan_g2p_strerror(s),
                  static_cast<int>(info.err_byte), inter.c_str());
        return false;
    }
    if (n_ids <= 3) return false;  // ^ _ $ だけ = 読める音が無い
    if (n_ids > kMaxIds) {
        SANO_LOGW("%d ids exceeds %d (split the text) — falling back", static_cast<int>(n_ids),
                  static_cast<int>(kMaxIds));
        return false;
    }
    ids.resize(static_cast<std::size_t>(n_ids));
    return true;
}

float tempo_scale(float mora_ms) {
    return std::clamp(mora_ms / kBaseMoraMs, kTempoMin, kTempoMax);
}

struct Synthesized {
    std::vector<float> pcm;  // 22.05 kHz、[-1, 1]
    float peak = 0.0f;
    std::int32_t n_frames = 0;
    // 正規化前の int16 (lrintf(x × 32767)) の FNV-1a 64 bit。上流 sanoTTS-jp の
    // QEMU / 実機記録と突き合わせるための移植検証値 (デモ文 "きょ][おわよ][いて][んきです°ね"、
    // mora_ms 110 で W8A8+PIE 0xa69a7ebbb5ccb05f / W8A32 0xe4b645c30835d42d)。
    std::uint64_t checksum = 1469598103934665603ull;
};

// ids → 22.05 kHz float PCM。合成中は arena を握る。
bool synthesize_pcm(const std::vector<std::int32_t>& ids, float s_v, Synthesized& out) {
    ArenaPtr arena = alloc_arena();
    if (!arena) {
        SANO_LOGW("arena %u B alloc failed", static_cast<unsigned>(kArenaBytes));
        return false;
    }
    saan_arena a;
    saan_arena_init(&a, arena.get(), kArenaBytes);

    const auto n_ids = static_cast<std::int32_t>(ids.size());
    saan_stream st;
    saan_status s = saan_stream_init(&st, &g_model.weights, &a, ids.data(), n_ids, s_v);
    if (s != SAAN_OK) {
        SANO_LOGW("saan_stream_init: %s", saan_strerror(s));
        return false;
    }
    // 二重防御 (上流 main.c): init が OK でも確保が黙って抜けていないか。
    if (a.used != saan_stream_arena_used(n_ids)) {
        SANO_LOGW("arena used %u B != expected %u B — refusing to pull",
                  static_cast<unsigned>(a.used), static_cast<unsigned>(saan_stream_arena_used(n_ids)));
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(st.n_frames) * SAAN_HOP;
    if (total == 0 || total > kMaxSamples) {
        SANO_LOGW("utterance %u samples out of range", static_cast<unsigned>(total));
        return false;
    }

    // 1 チャンク = 8 frame × 256 = 2,048 sample (8 KB)。スタックには置かない。
    std::vector<float> chunk(static_cast<std::size_t>(SAAN_CHUNK) * SAAN_HOP);
    out.pcm.clear();
    out.pcm.reserve(total);
    out.peak = 0.0f;
    out.n_frames = st.n_frames;
    for (;;) {
        std::int32_t n = 0;
        s = saan_stream_pull(&st, chunk.data(), &n);
        if (s != SAAN_OK) {
            SANO_LOGW("saan_stream_pull: %s", saan_strerror(s));
            return false;
        }
        if (n <= 0) break;
        const auto ns = static_cast<std::ptrdiff_t>(n) * SAAN_HOP;
        for (std::ptrdiff_t i = 0; i < ns; ++i) {
            out.peak = std::max(out.peak, std::fabs(chunk[i]));
            // 上流 saan_f32_to_i16 と同じ変換・同じハッシュ (FNV-1a、LE 2 バイト)
            long v = std::lrint(chunk[i] * 32767.0f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(v));
            out.checksum = (out.checksum ^ (u & 0xffu)) * 1099511628211ull;
            out.checksum = (out.checksum ^ (u >> 8)) * 1099511628211ull;
        }
        out.pcm.insert(out.pcm.end(), chunk.begin(), chunk.begin() + ns);
#if defined(ESP_PLATFORM)
        vTaskDelay(1);  // 1 pull ≈ 40〜150 ms。idle / 同優先度タスク (WDT 含む) に回す
#endif
    }
    return true;
}

// 発話のピークを目標レベルに合わせる倍率 (他エンジンと音量と口の開きを揃える)。
float normalize_scale(float peak, float gain) {
    if (peak <= kSilencePeak) return 1.0f;
    const float target = std::min(kPeakTarget * gain / kDefaultGain, kPeakCeiling);
    return std::min(target / peak, kMaxGainBoost);
}

internal::SincResampler g_resampler;

}  // namespace

namespace internal {

bool render_sano(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt,
                 std::uint32_t& out_rate_hz) {
    std::lock_guard<std::mutex> lock(g_model.mutex);
    if (!g_model.loaded) return false;

    std::vector<std::int32_t> ids;
    if (!text_to_ids(text, ids)) return false;

    const auto t0 = std::chrono::steady_clock::now();
    Synthesized synth;
    if (!synthesize_pcm(ids, SAAN_S_V * tempo_scale(opt.mora_ms), synth)) return false;
    const auto t1 = std::chrono::steady_clock::now();

    // 既定はモデル本来の 22.05 kHz のまま (上流と同じ、リサンプル無し)。
    const std::uint32_t out_rate = opt.sano_native_rate ? static_cast<std::uint32_t>(SAAN_SR)
                                                        : opt.sample_rate_hz;
    if (!g_resampler.prepare(SAAN_SR, out_rate)) {
        SANO_LOGW("unsupported output rate %u", static_cast<unsigned>(out_rate));
        return false;
    }
    const float scale = normalize_scale(synth.peak, opt.gain);
    const std::size_t out_before = out.size();
    g_resampler.run(synth.pcm.data(), synth.pcm.size(), scale, out);
    const auto t2 = std::chrono::steady_clock::now();

    const auto ms = [](auto d) {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    };
    const double audio_ms = 1000.0 * static_cast<double>(synth.pcm.size()) / SAAN_SR;
    SANO_LOGI("%u ids / %d frames / audio %.0f ms: synth %ld ms (xRT %.2f) + %s %ld ms "
              "→ %u samples @%u Hz, peak %.2f×%.2f",
              static_cast<unsigned>(ids.size()), static_cast<int>(synth.n_frames), audio_ms, ms(t1 - t0),
              audio_ms > 0 ? static_cast<double>(ms(t1 - t0)) / audio_ms : 0.0,
              out_rate == SAAN_SR ? "convert" : "resample", ms(t2 - t1),
              static_cast<unsigned>(out.size() - out_before), static_cast<unsigned>(out_rate),
              static_cast<double>(synth.peak), static_cast<double>(scale));
    SANO_LOGI("pcm checksum FNV-1a 0x%08lx%08lx (s_v %.4f)",
              static_cast<unsigned long>(synth.checksum >> 32),
              static_cast<unsigned long>(synth.checksum & 0xffffffffu),
              static_cast<double>(SAAN_S_V * tempo_scale(opt.mora_ms)));
    out_rate_hz = out_rate;
    return true;
}

}  // namespace internal

bool set_sano_model(std::span<const std::uint8_t> blob) {
    std::lock_guard<std::mutex> lock(g_model.mutex);
    g_model.loaded = false;
    if (blob.empty()) return true;
    if ((reinterpret_cast<std::uintptr_t>(blob.data()) % kArenaAlign) != 0u) {
        SANO_LOGW("blob is not 16-byte aligned (%p)", static_cast<const void*>(blob.data()));
        return false;
    }
    const saan_status s = saan_weights_open(&g_model.weights, blob.data(), blob.size());
    if (s != SAAN_OK) {
        SANO_LOGW("saan_weights_open: %s", saan_strerror(s));
        return false;
    }
    // コアは W8A8 でビルドしてある。fp32 blob だと int8 経路が 1 命令も効かないまま
    // 動いてしまうので、int8 blob だけが持つ scale テンソルで確かめる (上流 main.c)。
    if (saan_tensor(&g_model.weights, "duration.blocks.0.c1.weight.scale", nullptr, nullptr, nullptr) ==
        nullptr) {
        SANO_LOGW("blob is not the int8 model (no *.weight.scale tensors)");
        return false;
    }
    g_model.loaded = true;
    SANO_LOGI("model loaded: %u tensors / blob v%u", static_cast<unsigned>(g_model.weights.n_tensors),
              static_cast<unsigned>(g_model.weights.version));
    return true;
}

bool sano_model_loaded() {
    std::lock_guard<std::mutex> lock(g_model.mutex);
    return g_model.loaded;
}

}  // namespace stackchan::jtts

#else  // !JTTS_SANO_AVAILABLE — スタブ (推論コアをリンクしない)

namespace stackchan::jtts {

bool set_sano_model(std::span<const std::uint8_t>) { return false; }
bool sano_model_loaded() { return false; }

namespace internal {
bool render_sano(std::u32string_view, std::vector<std::int16_t>&, const Options&, std::uint32_t&) {
    return false;
}
}  // namespace internal

}  // namespace stackchan::jtts

#endif  // JTTS_SANO_AVAILABLE

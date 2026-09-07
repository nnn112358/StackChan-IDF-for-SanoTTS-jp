// SPDX-FileCopyrightText: 2026 nnn112358 <neko112358@gmail.com>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS-jp (蒸留ニューラル TTS、559 K params) エンジン。components/saanotts_core の
// C99 推論コアを jtts::Engine::Sano として使う。
//
//   かな (jtts 表記) ──▶ かな中間表現 (sano_text.cpp) ──▶ saan_g2p ──▶ ids
//     ──▶ saan_stream_init / pull (22.05 kHz float) ──▶ gain (既定 ×1.0)
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
#include "synth_dsp.hpp"

extern "C" {
#include "g2p.h"
#include "saanotts.h"
#include "saanotts_stream.h"
#if defined(SAAN_KANJI) && SAAN_KANJI
#include "saan_kanji.h"
#endif
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
// 学習モデルの話速が速めなので継続長に掛ける一律の補正 (CONFIG_JTTS_SANO_SPEED_PCT、
// 既定 125%)。ホスト ビルド (テスト / 上流 checksum の突き合わせ) は 100%。
#if defined(CONFIG_JTTS_SANO_SPEED_PCT)
constexpr float kSpeedCorrection = static_cast<float>(CONFIG_JTTS_SANO_SPEED_PCT) / 100.0f;
#else
constexpr float kSpeedCorrection = 1.0f;
#endif

// 音量: 上流 SanoTTS-jp-M5StackCoreS3 と同じく**正規化しない** (デモ文で |max| ≈ 0.29、
// 音量 128 で CoreS3 の内蔵スピーカーが歪まないレベル)。opt.gain は既定 0.6 に対する
// 相対倍率としてだけ掛ける (既定 = ×1.0 = 素通し)。
constexpr float kDefaultGain = 0.6f;

// ---- モデル ---------------------------------------------------------------

struct Model {
    saan_weights weights{};
    bool loaded = false;
    const void* dict = nullptr;  // jdict_t* (CONFIG_JTTS_SANO_KANJI のときだけ非 null になりうる)
    // 重みの差し替え (将来の HTTP アップロード) と合成を直列化する。合成は数秒保持する。
    std::mutex mutex;
};
Model g_model;

#if defined(SAAN_KANJI) && SAAN_KANJI
// 漢字 G2P の作業領域 (Viterbi 48 KB + 固定長配列 + label_ids の表) が合成 arena に収まること。
static_assert(kArenaBytes >= SAAN_KANJI_WORKBYTES, "sanoTTS arena is too small for the kanji G2P");
#endif

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

bool ids_ok(std::int32_t n_ids, std::vector<std::int32_t>& ids) {
    if (n_ids <= 3) return false;  // ^ _ $ だけ = 読める音が無い
    if (n_ids > kMaxIds) {
        SANO_LOGW("%d ids exceeds %d (split the text) — falling back", static_cast<int>(n_ids),
                  static_cast<int>(kMaxIds));
        return false;
    }
    ids.resize(static_cast<std::size_t>(n_ids));
    return true;
}

// 辞書経路が要るか: かな中間表現にできない文字 (漢字・英数字など) を含むとき。
// カタカナ・約物・空白・アクセント記号だけならかな経路で足りる。
bool needs_dictionary(std::u32string_view text) {
    std::string inter;
    std::size_t skipped = 0;
    internal::build_sano_intermediate(text, inter, &skipped);
    return skipped > 0;
}

#if defined(SAAN_KANJI) && SAAN_KANJI
// 漢字かな交じり文 → ids (Open JTalk)。jtts のアクセント記号 (' /) と sanoTTS の記号は落とし、
// UTF-8 の生文字列をそのまま辞書に渡す (句読点は Open JTalk が解釈する)。
bool kanji_to_ids(std::u32string_view text, std::vector<std::int32_t>& ids, std::uint8_t* arena) {
    std::string utf8;
    for (char32_t c : text) {
        if (c == U'\'' || c == U'’' || c == U'/' || c == U'[' || c == U']' || c == U'#' || c == U'_' ||
            c == U'^' || c == U'$' || c == U'°') {
            continue;
        }
        // append UTF-8
        if (c < 0x80) utf8.push_back(static_cast<char>(c));
        else if (c < 0x800) { utf8.push_back(static_cast<char>(0xC0 | (c >> 6))); utf8.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) { utf8.push_back(static_cast<char>(0xE0 | (c >> 12))); utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); utf8.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else { utf8.push_back(static_cast<char>(0xF0 | (c >> 18))); utf8.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F))); utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); utf8.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
    }
    if (utf8.empty()) return false;
    const std::int32_t cap = saan_g2p_capacity(utf8.size());
    ids.assign(static_cast<std::size_t>(cap), 0);
    std::int32_t n_ids = 0;
    int n_tok = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const saan_kanji_status ks = saan_kanji_to_ids(static_cast<const jdict_t*>(g_model.dict), utf8.data(), utf8.size(),
                                                   arena, kArenaBytes, ids.data(), cap, &n_ids, &n_tok);
    const auto t1 = std::chrono::steady_clock::now();
    if (ks != SAAN_KANJI_OK) {
        SANO_LOGW("kanji g2p failed: %s for \"%s\"", saan_kanji_strerror(ks), utf8.c_str());
        return false;
    }
    SANO_LOGI("kanji g2p: %u B → %d morphemes → %d ids (%ld ms)", static_cast<unsigned>(utf8.size()), n_tok,
              static_cast<int>(n_ids),
              static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
    return ids_ok(n_ids, ids);
}
#endif

// 読み → 音素 ID 列。読めない / 空 / 長すぎるは false。
// 辞書があり漢字などを含む読みは辞書経路 (arena を Viterbi の作業領域に借りる)、
// それ以外はかな中間表現 G2P。
bool text_to_ids(std::u32string_view text, std::vector<std::int32_t>& ids, std::uint8_t* arena) {
#if defined(SAAN_KANJI) && SAAN_KANJI
    if (g_model.dict != nullptr && arena != nullptr && needs_dictionary(text)) {
        return kanji_to_ids(text, ids, arena);
    }
#else
    (void)arena;
#endif
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
    return ids_ok(n_ids, ids);
}

float tempo_scale(float mora_ms) {
    return std::clamp(mora_ms / kBaseMoraMs, kTempoMin, kTempoMax) * kSpeedCorrection;
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

// ids → 22.05 kHz float PCM。arena は呼び出し側が渡す (G2P と共用)。
bool synthesize_pcm(const std::vector<std::int32_t>& ids, float s_v, std::uint8_t* arena_buf, Synthesized& out) {
    saan_arena a;
    saan_arena_init(&a, arena_buf, kArenaBytes);

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

internal::SincResampler g_resampler;

}  // namespace

namespace internal {

bool render_sano(std::u32string_view text, std::vector<std::int16_t>& out, const Options& opt,
                 std::uint32_t& out_rate_hz) {
    std::lock_guard<std::mutex> lock(g_model.mutex);
    if (!g_model.loaded) return false;

    ArenaPtr arena = alloc_arena();
    if (!arena) {
        SANO_LOGW("arena %u B alloc failed", static_cast<unsigned>(kArenaBytes));
        return false;
    }
    std::vector<std::int32_t> ids;
    if (!text_to_ids(text, ids, arena.get())) return false;

    const auto t0 = std::chrono::steady_clock::now();
    Synthesized synth;
    if (!synthesize_pcm(ids, SAAN_S_V * tempo_scale(opt.mora_ms), arena.get(), synth)) return false;
    const auto t1 = std::chrono::steady_clock::now();

    // 既定はモデル本来の 22.05 kHz のまま (上流と同じ、リサンプル無し)。
    const std::uint32_t out_rate = opt.sano_native_rate ? static_cast<std::uint32_t>(SAAN_SR)
                                                        : opt.sample_rate_hz;
    if (!g_resampler.prepare(SAAN_SR, out_rate)) {
        SANO_LOGW("unsupported output rate %u", static_cast<unsigned>(out_rate));
        return false;
    }
    const float scale = opt.gain / kDefaultGain;  // 既定 gain なら素通し
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

// ---- ストリーミング API ----------------------------------------------------------

struct SanoStream::Impl {
    std::unique_lock<std::mutex> lock;   // begin() 〜 end() でモデルを占有
    ArenaPtr arena;
    saan_arena a{};
    saan_stream st{};
    std::vector<std::int32_t> ids;
    std::vector<float> chunk;
    std::size_t total = 0;
    float scale = 1.0f;
    bool active = false;
};

SanoStream::SanoStream() : impl_(std::make_unique<Impl>()) {}
SanoStream::~SanoStream() { end(); }

bool SanoStream::begin(std::u32string_view kana, const Options& opt) {
    end();
    std::unique_lock<std::mutex> lock(g_model.mutex);
    if (!g_model.loaded) return false;
    impl_->arena = alloc_arena();
    if (!impl_->arena) {
        SANO_LOGW("arena %u B alloc failed", static_cast<unsigned>(kArenaBytes));
        return false;
    }
    if (!text_to_ids(kana, impl_->ids, impl_->arena.get())) return false;
    saan_arena_init(&impl_->a, impl_->arena.get(), kArenaBytes);
    const auto n_ids = static_cast<std::int32_t>(impl_->ids.size());
    const float s_v = SAAN_S_V * tempo_scale(opt.mora_ms);
    const saan_status s = saan_stream_init(&impl_->st, &g_model.weights, &impl_->a, impl_->ids.data(), n_ids, s_v);
    if (s != SAAN_OK) {
        SANO_LOGW("saan_stream_init: %s", saan_strerror(s));
        return false;
    }
    if (impl_->a.used != saan_stream_arena_used(n_ids)) {
        SANO_LOGW("arena used %u B != expected %u B — refusing to pull",
                  static_cast<unsigned>(impl_->a.used), static_cast<unsigned>(saan_stream_arena_used(n_ids)));
        return false;
    }
    impl_->total = static_cast<std::size_t>(impl_->st.n_frames) * SAAN_HOP;
    if (impl_->total == 0 || impl_->total > kMaxSamples) {
        SANO_LOGW("utterance %u samples out of range", static_cast<unsigned>(impl_->total));
        return false;
    }
    impl_->chunk.assign(static_cast<std::size_t>(SAAN_CHUNK) * SAAN_HOP, 0.0f);
    impl_->scale = opt.gain / kDefaultGain;
    impl_->lock = std::move(lock);
    impl_->active = true;
    SANO_LOGI("stream: %d ids / %d frames / %u samples (%.0f ms) @%d Hz", static_cast<int>(n_ids),
              static_cast<int>(impl_->st.n_frames), static_cast<unsigned>(impl_->total),
              1000.0 * static_cast<double>(impl_->total) / SAAN_SR, static_cast<int>(SAAN_SR));
    return true;
}

std::size_t SanoStream::total_samples() const { return impl_->active ? impl_->total : 0; }
std::uint32_t SanoStream::sample_rate() const { return static_cast<std::uint32_t>(SAAN_SR); }

int SanoStream::pull(std::int16_t* out, std::size_t cap) {
    if (!impl_->active || out == nullptr || cap < kChunkSamples) return -1;
    std::int32_t n = 0;
    const saan_status s = saan_stream_pull(&impl_->st, impl_->chunk.data(), &n);
    if (s != SAAN_OK) {
        SANO_LOGW("saan_stream_pull: %s", saan_strerror(s));
        return -1;
    }
    if (n <= 0) return 0;
    const std::size_t ns = static_cast<std::size_t>(n) * SAAN_HOP;
    for (std::size_t i = 0; i < ns; ++i) out[i] = internal::dsp::to_i16(impl_->chunk[i] * impl_->scale);
    return static_cast<int>(ns);
}

void SanoStream::end() {
    if (!impl_ || !impl_->active) return;
    impl_->active = false;
    impl_->arena.reset();
    impl_->chunk.clear();
    impl_->chunk.shrink_to_fit();
    if (impl_->lock.owns_lock()) impl_->lock.unlock();
}

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

bool set_sano_dict(const void* jdict) {
#if defined(SAAN_KANJI) && SAAN_KANJI
    std::lock_guard<std::mutex> lock(g_model.mutex);
    g_model.dict = jdict;
    if (jdict != nullptr && saan_kanji_init() == 0) {
        g_model.dict = nullptr;
        return false;
    }
    return true;
#else
    (void)jdict;
    return false;
#endif
}

bool sano_dict_loaded() {
    std::lock_guard<std::mutex> lock(g_model.mutex);
    return g_model.dict != nullptr;
}

}  // namespace stackchan::jtts

#else  // !JTTS_SANO_AVAILABLE — スタブ (推論コアをリンクしない)

namespace stackchan::jtts {

bool set_sano_model(std::span<const std::uint8_t>) { return false; }
bool sano_model_loaded() { return false; }
bool set_sano_dict(const void*) { return false; }
bool sano_dict_loaded() { return false; }

struct SanoStream::Impl {};
SanoStream::SanoStream() = default;
SanoStream::~SanoStream() = default;
bool SanoStream::begin(std::u32string_view, const Options&) { return false; }
std::size_t SanoStream::total_samples() const { return 0; }
std::uint32_t SanoStream::sample_rate() const { return 22050; }
int SanoStream::pull(std::int16_t*, std::size_t) { return -1; }
void SanoStream::end() {}

namespace internal {
bool render_sano(std::u32string_view, std::vector<std::int16_t>&, const Options&, std::uint32_t&) {
    return false;
}
}  // namespace internal

}  // namespace stackchan::jtts

#endif  // JTTS_SANO_AVAILABLE

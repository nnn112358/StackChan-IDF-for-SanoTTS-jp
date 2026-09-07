// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS エンジンのホスト単体テスト。
//   jtts_test_sano [model.bin]
// 引数無しなら読み変換とリサンプラだけ、blob を渡すと合成まで検査する。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"
#include "resampler.hpp"

using namespace stackchan::jtts;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool inter_is(std::u32string_view in, std::string_view expect, std::size_t expect_skipped = 0) {
    std::string out;
    std::size_t skipped = 0;
    const bool ok = internal::build_sano_intermediate(in, out, &skipped);
    if (!ok || out != expect || skipped != expect_skipped) {
        std::printf("       got \"%s\" (skipped %zu), expected \"%.*s\" (skipped %zu)\n", out.c_str(), skipped,
                    static_cast<int>(expect.size()), expect.data(), expect_skipped);
        return false;
    }
    return true;
}

void test_intermediate() {
    check(inter_is(U"きょ][おわよ][いて][んきです°ね", "きょ][おわよ][いて][んきです°ね"), "sanoTTS marks pass through");
    check(inter_is(U"ナデナデ シテー", "なでなで#してー"), "katakana → hiragana, space → #");
    check(inter_is(U"こんにちは、すたっくちゃんです。", "こんにちは#すたっくちゃんです"), "punctuation → #, trailing # trimmed");
    check(inter_is(U"きょ'うの/て'んき", "きょ]うの#て]んき"), "jtts accent ' and / mapped");
    check(inter_is(U"げんき？！", "げんき?!"), "question + suffix kept as 2-char mark");
    check(inter_is(U"、、あ。。", "あ"), "leading / doubled boundaries dropped");
    check(inter_is(U"元気げんき", "げんき", 2), "kanji skipped and counted");
    std::string out;
    check(!internal::build_sano_intermediate(U"漢字", out, nullptr), "nothing pronounceable → false");
}

void test_resampler() {
    internal::SincResampler rs;
    check(rs.prepare(22050, 16000), "prepare 22050 → 16000");
    // 1 秒の 1 kHz 正弦波: 長さ比とピーク保存
    std::vector<float> in(22050);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = std::sin(2.0f * std::numbers::pi_v<float> * 1000.0f * static_cast<float>(i) / 22050.0f);
    }
    // 端はカーネルが切り詰められる (ゼロ埋め) ので、ピークは端を除いて測る。
    auto peak_inside = [](const std::vector<std::int16_t>& v) {
        int peak = 0;
        for (std::size_t i = 100; i + 100 < v.size(); ++i) peak = std::max(peak, std::abs(static_cast<int>(v[i])));
        return peak;
    };
    std::vector<std::int16_t> out;
    rs.run(in.data(), in.size(), 0.5f, out);
    check(out.size() == 16000, "output length = n_in × 320 / 441");
    int peak = peak_inside(out);
    const double ratio = static_cast<double>(peak) / (0.5 * 32760.0);
    std::printf("       1 kHz peak ratio %.3f\n", ratio);
    check(ratio > 0.97 && ratio < 1.03, "1 kHz tone amplitude preserved (±3%)");
    // 10 kHz (出力ナイキスト超) は減衰する
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = std::sin(2.0f * std::numbers::pi_v<float> * 10000.0f * static_cast<float>(i) / 22050.0f);
    }
    out.clear();
    rs.run(in.data(), in.size(), 1.0f, out);
    peak = peak_inside(out);
    std::printf("       10 kHz residual %.4f\n", peak / 32760.0);
    check(peak < 32760 * 0.03, "10 kHz (above output Nyquist) attenuated below -30 dB");
    // 同一レートは素通し
    internal::SincResampler same;
    check(same.prepare(16000, 16000) && same.output_length(123) == 123, "same rate passthrough");
}

void test_synthesis(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        check(false, "open model blob");
        return;
    }
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> blob(raw.size() + 16);
    auto* base = blob.data();
    while ((reinterpret_cast<std::uintptr_t>(base) & 15u) != 0u) ++base;
    std::memcpy(base, raw.data(), raw.size());
    check(set_sano_model({base, raw.size()}), "set_sano_model");
    check(sano_model_loaded(), "sano_model_loaded");

    Options opt;
    opt.engine = Engine::Sano;
    opt.gain = 0.6f;
    std::vector<std::int16_t> pcm;
    std::uint32_t rate = 0;
    // 上流のデモ文 (53 ids / 106 frames が既知の値)。既定はネイティブ 22.05 kHz。
    auto r = synthesize(U"きょ][おわよ][いて][んきです°ね", pcm, opt, &rate);
    check(static_cast<bool>(r), "synthesize demo sentence");
    check(rate == 22050, "native rate reported as 22050 Hz");
    check(pcm.size() == static_cast<std::size_t>(106) * 256, "sample count = 106 frames × 256 (no resample)");
    // リサンプル経路 (sano_native_rate = false): 106 frames × 256 @22.05 kHz → 16 kHz
    Options opt16 = opt;
    opt16.sano_native_rate = false;
    std::vector<std::int16_t> pcm16;
    std::uint32_t rate16 = 0;
    check(static_cast<bool>(synthesize(U"きょ][おわよ][いて][んきです°ね", pcm16, opt16, &rate16)) &&
              rate16 == 16000, "resample path reports 16000 Hz");
    const std::size_t expect = static_cast<std::size_t>(106) * 256 * 16000 / 22050;
    std::printf("       %zu samples (expected %zu)\n", pcm16.size(), expect);
    check(pcm16.size() == expect, "sample count matches 106 frames resampled to 16 kHz");
    int peak = 0;
    for (auto v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
    std::printf("       peak %d\n", peak);
    check(peak > 0.28 * 32760 && peak <= 0.3 * 32760 + 8, "peak normalised to 0.3 at default gain 0.6");

    // Auto はモデルがあれば Sano を選ぶ (レート 22050 で判別)
    Options auto_opt;
    std::vector<std::int16_t> pcm_auto;
    std::uint32_t rate_auto = 0;
    check(static_cast<bool>(synthesize(U"きょ][おわよ][いて][んきです°ね", pcm_auto, auto_opt, &rate_auto)) &&
              pcm_auto.size() == pcm.size() && rate_auto == 22050,
          "Engine::Auto picks Sano when the model is loaded");

    // 読めない文はフォールバック (Sano は false、Formant が鳴る)
    std::vector<std::int16_t> pcm_fb;
    check(static_cast<bool>(synthesize(U"漢字だけ", pcm_fb, auto_opt)) == false || !pcm_fb.empty(),
          "unpronounceable text falls through without crashing");

    check(set_sano_model({}), "unload model");
    check(!sano_model_loaded(), "unloaded");
}

}  // namespace

int main(int argc, char** argv) {
    test_intermediate();
    test_resampler();
    if (argc >= 2) {
        test_synthesis(argv[1]);
    } else {
        std::printf("[skip] synthesis (pass the model blob path to enable)\n");
    }
    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

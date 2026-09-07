// SPDX-FileCopyrightText: 2026 nnn112358 <neko112358@gmail.com>
// SPDX-License-Identifier: BSL-1.0
#include "resampler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

#include "synth_dsp.hpp"

namespace stackchan::jtts::internal {

namespace {
// 係数表の上限 (位相数)。16 kHz ↔ 22.05 kHz は 320、48 kHz ↔ 22.05 kHz は 320。
constexpr std::uint32_t kMaxPhases = 4096;
}  // namespace

bool SincResampler::prepare(std::uint32_t in_rate, std::uint32_t out_rate) {
    if (in_rate == 0 || out_rate == 0) return false;
    if (in_rate == in_rate_ && out_rate == out_rate_) return true;

    const std::uint32_t g = std::gcd(in_rate, out_rate);
    const std::uint32_t step_num = in_rate / g;
    const std::uint32_t phases = out_rate / g;
    if (phases > kMaxPhases) return false;

    std::vector<float> table;
    if (in_rate != out_rate) {
        // 入力レート正規化のカットオフ (低い方のナイキスト × 0.9)
        const float fc = 0.45f * static_cast<float>(std::min(in_rate, out_rate)) /
                         static_cast<float>(in_rate);
        table.assign(static_cast<std::size_t>(phases) * kTaps, 0.0f);
        for (std::uint32_t p = 0; p < phases; ++p) {
            const float frac = static_cast<float>(p) / static_cast<float>(phases);
            float* h = &table[static_cast<std::size_t>(p) * kTaps];
            float sum = 0.0f;
            for (int j = 0; j < kTaps; ++j) {
                // 入力 sample (idx + j - kHalf + 1) と出力位置 (idx + frac) の距離
                const float x = static_cast<float>(j - kHalf + 1) - frac;
                const float window =
                    0.5f + 0.5f * std::cos(std::numbers::pi_v<float> * x / static_cast<float>(kHalf + 1));
                const float sinc = (std::fabs(x) < 1e-6f)
                                       ? 2.0f * fc
                                       : std::sin(2.0f * std::numbers::pi_v<float> * fc * x) /
                                             (std::numbers::pi_v<float> * x);
                h[j] = sinc * window;
                sum += h[j];
            }
            for (int j = 0; j < kTaps; ++j) h[j] /= sum;  // DC ゲイン 1
        }
    }
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    step_num_ = step_num;
    phases_ = phases;
    table_ = std::move(table);
    return true;
}

std::size_t SincResampler::output_length(std::size_t n_in) const {
    if (in_rate_ == 0) return 0;
    return static_cast<std::size_t>(static_cast<std::uint64_t>(n_in) * out_rate_ / in_rate_);
}

void SincResampler::run(const float* in, std::size_t n_in, float scale,
                        std::vector<std::int16_t>& out) const {
    if (in_rate_ == out_rate_) {
        out.reserve(out.size() + n_in);
        for (std::size_t i = 0; i < n_in; ++i) out.push_back(dsp::to_i16(in[i] * scale));
        return;
    }
    const std::size_t n_out = output_length(n_in);
    out.reserve(out.size() + n_out);
    // 出力 n の入力位置 = n × step_num_ / phases_。整数部 idx と位相を逐次更新する。
    std::size_t idx = 0;
    std::uint32_t phase = 0;
    for (std::size_t n = 0; n < n_out; ++n) {
        const float* h = &table_[static_cast<std::size_t>(phase) * kTaps];
        float acc = 0.0f;
        for (int j = 0; j < kTaps; ++j) {
            const long k = static_cast<long>(idx) + (j - kHalf + 1);
            if (k >= 0 && static_cast<std::size_t>(k) < n_in) acc += h[j] * in[k];
        }
        out.push_back(dsp::to_i16(acc * scale));
        phase += step_num_;
        idx += phase / phases_;
        phase %= phases_;
    }
}

}  // namespace stackchan::jtts::internal

// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 有理比 (L/M) の窓付き sinc リサンプラ。sanoTTS の 22.05 kHz 出力を jtts の出力レート
// (16 kHz) に合わせるために使う。出力位相は M 通りしか無いので、位相ごとの係数表
// (M × taps) を prepare() で一度だけ作り、run() は表引きの積和だけ。
// カットオフは低い方のナイキストの 0.9 倍 (16 kHz なら 7.2 kHz — HMM のデシメータと同じ)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stackchan::jtts::internal {

class SincResampler {
public:
    static constexpr int kHalf = 16;         // 片側タップ数
    static constexpr int kTaps = 2 * kHalf;  // 32 タップ (出力 1 sample あたり 32 MAC)

    // 係数表を作る。同じレート対なら何もしない。位相数 (out/gcd) が大きすぎる比は false。
    // in_rate == out_rate も許す (run() が素通しになる)。
    bool prepare(std::uint32_t in_rate, std::uint32_t out_rate);

    // n_in 入力サンプルに対する出力サンプル数。
    std::size_t output_length(std::size_t n_in) const;

    // in[0..n_in) を scale 倍してリサンプルし、int16 で out に追記する。
    void run(const float* in, std::size_t n_in, float scale, std::vector<std::int16_t>& out) const;

    std::uint32_t in_rate() const { return in_rate_; }
    std::uint32_t out_rate() const { return out_rate_; }

private:
    std::uint32_t in_rate_ = 0, out_rate_ = 0;
    std::uint32_t step_num_ = 0;  // 出力 1 sample あたり進む入力 sample (位相単位)
    std::uint32_t phases_ = 0;    // 位相数
    std::vector<float> table_;    // [phases_][kTaps]
};

}  // namespace stackchan::jtts::internal

// SPDX-FileCopyrightText: 2026 nnn112358 <neko112358@gmail.com>
// SPDX-License-Identifier: BSL-1.0
//
// sanoTTS エンジンのホスト デモ / 検証 CLI。
//   jtts_sano_demo inter <かな>                                  … かな中間表現を stdout に出す
//   jtts_sano_demo synth <model.bin> <かな> <out.wav> [mora_ms] [rate_hz]
// ホストは PIE 無しの W8A8 スカラ経路なので実機 (PIE) とは bit 一致しない (音は同じ)。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jtts.hpp"
#include "wav_writer.hpp"

extern "C" {
#include "jdict.h"
}

using namespace stackchan::jtts;

namespace {

std::u32string to_u32(const char* utf8) {
    std::u32string out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8);
    while (*p != 0) {
        char32_t cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p >> 5) == 0x6) {
            cp = (*p++ & 0x1F) << 6;
            cp |= (*p++ & 0x3F);
        } else if ((*p >> 4) == 0xE) {
            cp = (*p++ & 0x0F) << 12;
            cp |= (*p++ & 0x3F) << 6;
            cp |= (*p++ & 0x3F);
        } else {
            cp = (*p++ & 0x07) << 18;
            cp |= (*p++ & 0x3F) << 12;
            cp |= (*p++ & 0x3F) << 6;
            cp |= (*p++ & 0x3F);
        }
        out.push_back(cp);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "inter") == 0) {
        std::string inter;
        if (!internal::build_sano_intermediate(to_u32(argv[2]), inter)) {
            std::fprintf(stderr, "nothing pronounceable\n");
            return 1;
        }
        std::printf("%s\n", inter.c_str());
        return 0;
    }
    if (argc >= 6 && std::strcmp(argv[1], "synthk") == 0) {
        // 辞書付き: jtts_sano_demo synthk <model.bin> <dict.bin> <漢字かな交じり文> <out.wav>
        std::ifstream f(argv[2], std::ios::binary);
        std::ifstream fd(argv[3], std::ios::binary);
        if (!f || !fd) { std::fprintf(stderr, "cannot open model / dict\n"); return 1; }
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::uint8_t> blob(raw.size() + 16);
        auto* base = blob.data();
        while ((reinterpret_cast<std::uintptr_t>(base) & 15u) != 0u) ++base;
        std::memcpy(base, raw.data(), raw.size());
        if (!set_sano_model({base, raw.size()})) { std::fprintf(stderr, "model load failed\n"); return 1; }
        static std::vector<std::uint8_t> dict_raw((std::istreambuf_iterator<char>(fd)), std::istreambuf_iterator<char>());
        static std::vector<std::uint8_t> dict_blob(dict_raw.size() + 16);
        auto* dbase = dict_blob.data();
        while ((reinterpret_cast<std::uintptr_t>(dbase) & 15u) != 0u) ++dbase;
        std::memcpy(dbase, dict_raw.data(), dict_raw.size());
        static jdict_t dict;
        if (jdict_open(&dict, dbase, dict_raw.size()) != 0) { std::fprintf(stderr, "jdict_open failed\n"); return 1; }
        if (!set_sano_dict(&dict)) { std::fprintf(stderr, "set_sano_dict failed\n"); return 1; }
        Options opt;
        opt.engine = Engine::Sano;
        std::vector<std::int16_t> pcm;
        std::uint32_t rate = 0;
        auto r = synthesize(to_u32(argv[4]), pcm, opt, &rate);
        if (!r) { std::fprintf(stderr, "synthesize failed: %s\n", to_string(r.error())); return 1; }
        std::printf("%zu samples @%u Hz (%.2f s)\n", pcm.size(), rate, static_cast<double>(pcm.size()) / rate);
        return write_wav_mono16(argv[5], pcm, rate) ? 0 : 1;
    }
    if (argc >= 5 && std::strcmp(argv[1], "synth") == 0) {
        std::ifstream f(argv[2], std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", argv[2]);
            return 1;
        }
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
        // 16 バイト境界 (PIE の要件。ホストでも同じ検査を通す)
        std::vector<std::uint8_t> blob(raw.size() + 16);
        auto* base = blob.data();
        while ((reinterpret_cast<std::uintptr_t>(base) & 15u) != 0u) ++base;
        std::memcpy(base, raw.data(), raw.size());
        if (!set_sano_model({base, raw.size()})) {
            std::fprintf(stderr, "model load failed\n");
            return 1;
        }
        Options opt;
        opt.engine = Engine::Sano;
        opt.gain = 0.8f;
        if (argc >= 6) opt.mora_ms = std::strtof(argv[5], nullptr);
        if (argc >= 7) opt.sample_rate_hz = static_cast<std::uint32_t>(std::strtoul(argv[6], nullptr, 10));
        // rate_hz を明示したときだけリサンプル経路、省略時はネイティブ 22.05 kHz
        if (argc >= 7) opt.sano_native_rate = false;
        std::vector<std::int16_t> pcm;
        std::uint32_t rate = 0;
        auto r = synthesize(to_u32(argv[3]), pcm, opt, &rate);
        if (!r) {
            std::fprintf(stderr, "synthesize failed: %s\n", to_string(r.error()));
            return 1;
        }
        int peak = 0;
        for (auto v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
        std::printf("%zu samples @%u Hz (%.2f s), peak %d\n", pcm.size(), rate,
                    static_cast<double>(pcm.size()) / rate, peak);
        if (!write_wav_mono16(argv[4], pcm, rate)) {
            std::fprintf(stderr, "cannot write %s\n", argv[4]);
            return 1;
        }
        return 0;
    }
    std::fprintf(stderr,
                 "usage: %s inter <kana>\n"
                 "       %s synth <model.bin> <kana> <out.wav> [mora_ms] [rate_hz]\n",
                 argv[0], argv[0]);
    return 2;
}

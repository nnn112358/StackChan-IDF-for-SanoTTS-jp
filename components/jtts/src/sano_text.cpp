// SPDX-FileCopyrightText: 2026 @nnn112358
// SPDX-License-Identifier: BSL-1.0
//
// jtts の読み (かな + アクセント記号) → sanoTTS のかな中間表現 (UTF-8)。
// 純粋な文字列変換で ESP-IDF にも推論コアにも依存しないので、常にコンパイルする
// (ホスト テストの対象)。
//
//   ひらがな / ー         … そのまま
//   カタカナ              … ひらがなに直す
//   '  ’                  … `]` (jtts: 直前モーラがアクセント核 → 下降)
//   /  、。，,．.  空白    … `#` (句境界。連続させず、先頭と末尾には置かない)
//   ? ？ (+ ! . ~ 全角可)  … `?` `?!` `?.` `?~` (疑問の EOS)
//   ! ！                  … 読み飛ばす (hts_label と同じ)
//   ° [ ] # _ ^ $         … sanoTTS の記号としてそのまま通す
//   それ以外 (漢字など)    … 読み飛ばして数える (他エンジンの parse_kana と同じ扱い)
#include <string>
#include <string_view>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

constexpr bool is_hiragana(char32_t c) { return c >= U'ぁ' && c <= U'ゖ'; }
constexpr bool is_katakana(char32_t c) { return c >= U'ァ' && c <= U'ヶ'; }
constexpr bool is_space(char32_t c) {
    return c == U' ' || c == U'　' || c == U'\n' || c == U'\r' || c == U'\t';
}
// hts_label.cpp と同じ「間」の約物 + jtts のアクセント句切り `/`。
constexpr bool is_boundary(char32_t c) {
    return c == U'/' || c == U'、' || c == U'。' || c == U'，' || c == U',' || c == U'．' ||
           c == U'.' || is_space(c);
}
constexpr bool is_question(char32_t c) { return c == U'?' || c == U'？'; }
// `?` の直後に付けて 2 文字マークにする接尾 (`?!` `?.` `?~`)。全角も受ける。0 = 該当なし。
constexpr char question_suffix(char32_t c) {
    switch (c) {
        case U'!': case U'！': return '!';
        case U'.': case U'．': return '.';
        case U'~': case U'～': return '~';
        default: return 0;
    }
}
constexpr bool is_accent_nucleus(char32_t c) { return c == U'\'' || c == U'’'; }
constexpr bool is_exclamation(char32_t c) { return c == U'!' || c == U'！'; }
// sanoTTS の中間表現マークのうち、そのまま通すもの (`#` と `?` は別扱い)。
constexpr bool is_passthrough_mark(char32_t c) {
    return c == U'°' || c == U'[' || c == U']' || c == U'_' || c == U'^' || c == U'$';
}

}  // namespace

bool build_sano_intermediate(std::u32string_view text, std::string& out, std::size_t* skipped) {
    out.clear();
    std::size_t n_skipped = 0;
    auto push_boundary = [&out]() {
        if (!out.empty() && out.back() != '#') out.push_back('#');
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char32_t ch = text[i];
        if (is_hiragana(ch) || ch == U'ー') {
            append_utf8(out, ch);
        } else if (is_katakana(ch)) {
            append_utf8(out, ch - (U'ァ' - U'ぁ'));
        } else if (is_accent_nucleus(ch)) {
            out.push_back(']');
        } else if (is_boundary(ch) || ch == U'#') {
            push_boundary();
        } else if (is_question(ch)) {
            out.push_back('?');
            if (i + 1 < text.size()) {
                if (const char suffix = question_suffix(text[i + 1]); suffix != 0) {
                    out.push_back(suffix);
                    ++i;
                }
            }
        } else if (is_exclamation(ch)) {
            continue;
        } else if (is_passthrough_mark(ch)) {
            append_utf8(out, ch);
        } else {
            ++n_skipped;
        }
    }
    while (!out.empty() && out.back() == '#') out.pop_back();
    if (skipped != nullptr) *skipped = n_skipped;
    return !out.empty();
}

}  // namespace stackchan::jtts::internal

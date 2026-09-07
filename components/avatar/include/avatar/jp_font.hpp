// SPDX-FileCopyrightText: 2026 @nnn112358
// SPDX-License-Identifier: BSL-1.0
//
// 日本語フォントの選択。通常は 12/16/20/24 px の 4 サイズ (合計 ~760 KB)。
// CONFIG_STACKCHAN_JP_FONT_16_ONLY のときは全部 16 px に畳んで flash を ~570 KB 節約する
// (辞書入り単一アプリ構成 cores3-dict 用)。
#pragma once

#include <M5GFX.h>

#include "sdkconfig.h"

namespace stackchan::avatar {

#if defined(CONFIG_STACKCHAN_JP_FONT_16_ONLY)
inline const lgfx::IFont* jp_font_12() { return &fonts::lgfxJapanGothic_16; }
inline const lgfx::IFont* jp_font_16() { return &fonts::lgfxJapanGothic_16; }
inline const lgfx::IFont* jp_font_20() { return &fonts::lgfxJapanGothic_16; }
inline const lgfx::IFont* jp_font_24() { return &fonts::lgfxJapanGothic_16; }
#else
inline const lgfx::IFont* jp_font_12() { return &fonts::lgfxJapanGothic_12; }
inline const lgfx::IFont* jp_font_16() { return &fonts::lgfxJapanGothic_16; }
inline const lgfx::IFont* jp_font_20() { return &fonts::lgfxJapanGothic_20; }
inline const lgfx::IFont* jp_font_24() { return &fonts::lgfxJapanGothic_24; }
#endif

} // namespace stackchan::avatar

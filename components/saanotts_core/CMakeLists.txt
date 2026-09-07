# sanoTTS-jp の C99 推論コア + かな中間表現 G2P (sanoTTS-jp/csrc からの vendored コピー)。
#
# 出所: https://github.com/ayutaz/sanoTTS-jp  origin/main d169e91 (2026-09-04) — MIT (LICENSE)。
#       中継: ~/Desktop/sanoTTS/esp32_cores3_tts (nnn112358/SanoTTS-jp-M5StackCoreS3)。
# 取り込んだのは推論に要る 4 ファイル (saanotts.c / saanotts_stream.c / fft.c / saanotts_int8.c)
# と端末側 G2P (g2p.c)。端末内漢字 G2P (Open JTalk + 辞書 13.7 MB) は flash に収まらないので
# 持ってこない — 入力は**かな中間表現** (ひらがな + アクセント記号) だけ。
#
# 利用側は components/jtts/src/sano_synth.cpp (jtts::Engine::Sano)。重み blob は
# assets/sanotts/saanotts-jp-v3-int8.bin を `sano` パーティションに焼いて mmap する。
#
# ⚠️ 上流と乖離しうる。上流 csrc/ を直したらここにも持ってくること (golden test は上流側)。

idf_component_register(
    SRCS "saanotts.c"
         "saanotts_stream.c"
         "fft.c"
         "saanotts_int8.c"
         "g2p.c"
    INCLUDE_DIRS "."
    REQUIRES ""
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

# プラットフォームの注入点: erf 表 (1 KB) を内部 DRAM に置く (saan_port_esp32.h)。
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SAAN_PORT_HEADER=\"saan_port_esp32.h\"")

# W8A8 + PIE (ESP32-S3 の整数 SIMD)。他ターゲットでは W8A8 のスカラ経路。
# ⚠️ SAAN_INT8_ACT と SAAN_PIE は対で渡す — PIE カーネルは W8A8 経路の中にしか無い。
if(CONFIG_IDF_TARGET_ESP32S3)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1 SAAN_PIE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1)
endif()

# 推論コアの .text は flash のまま (上流は linker.lf で IRAM に置いて -2.4% だが、この
# ファームは内部 D/IRAM が逼迫しているので使わない)。

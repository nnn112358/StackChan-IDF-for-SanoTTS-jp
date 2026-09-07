# sanoTTS-jp の C99 推論コア + かな中間表現 G2P (+ 端末内漢字 G2P) — sanoTTS-jp/csrc からの
# vendored コピー。
#
# 出所: https://github.com/ayutaz/sanoTTS-jp  origin/main d169e91 (2026-09-04) — MIT (LICENSE)。
#       中継: ~/Desktop/sanoTTS/esp32_cores3_tts (nnn112358/SanoTTS-jp-M5StackCoreS3)。
#   saanotts.c saanotts_stream.c fft.c saanotts_int8.c  … 推論コア
#   g2p.c                                              … かな中間表現 → ids
#   jdict.c accent.c njd_rules.c label_ids.c openjtalk/  … 端末内漢字 G2P (CONFIG_JTTS_SANO_KANJI)
#   saan_kanji.c (上流 esp32/main) / saan_dict.c (dict パーティションの mmap、ESP のみ)
#   oj_heap_psram.c … Open JTalk の calloc/strdup/free を PSRAM 優先に (-include で差し替え)
#
# 利用側は components/jtts/src/sano_synth.cpp (jtts::Engine::Sano)。重み blob は
# assets/sanotts/saanotts-jp-v3-int8.bin を `sano` パーティションに、辞書 (13.7 MB) は
# assets/sanotts/k1-dict-438750.bin を `dict` パーティションに焼いて mmap する
# (辞書は cores3-dict プロファイル = partitions_16mb_dict.csv のときだけ)。
#
# ⚠️ 上流と乖離しうる。上流 csrc/ を直したらここにも持ってくること (golden test は上流側)。

set(_saan_srcs "saanotts.c" "saanotts_stream.c" "fft.c" "saanotts_int8.c" "g2p.c")
set(_saan_inc ".")
set(_saan_k_srcs "")
if(CONFIG_JTTS_SANO_KANJI)
    file(GLOB _saan_ojt "${CMAKE_CURRENT_LIST_DIR}/openjtalk/*.c")
    set(_saan_k_srcs "jdict.c" "accent.c" "njd_rules.c" "label_ids.c" "saan_kanji.c" "saan_dict.c"
                     "oj_heap_psram.c" ${_saan_ojt})
    list(APPEND _saan_inc "openjtalk")
endif()

idf_component_register(
    SRCS ${_saan_srcs} ${_saan_k_srcs}
    INCLUDE_DIRS ${_saan_inc}
    REQUIRES ""
    PRIV_REQUIRES esp_partition esp_mm
)

# 推論コアは速度優先: プロファイルが -Os (cores3-dict) でもこのコンポーネントだけ -O2。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -O2)

# プラットフォームの注入点: erf 表 (1 KB) を内部 DRAM に置く (saan_port_esp32.h)。
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SAAN_PORT_HEADER=\"saan_port_esp32.h\"")

# W8A8 + PIE (ESP32-S3 の整数 SIMD)。他ターゲットでは W8A8 のスカラ経路。
# ⚠️ SAAN_INT8_ACT と SAAN_PIE は対で渡す — PIE カーネルは W8A8 経路の中にしか無い。
if(CONFIG_IDF_TARGET_ESP32S3)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1 SAAN_PIE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1)
endif()

if(CONFIG_JTTS_SANO_KANJI)
    # CHARSET_UTF_8 は Open JTalk が要求する。LABEL_IDS_EXTERNAL_SCRATCH=1 で label_ids の
    # トークン表 10 KB を .bss ではなく合成 arena から借りる (saan_kanji.c が layout() で渡す)。
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CHARSET_UTF_8 SAAN_KANJI=1 LABEL_IDS_EXTERNAL_SCRATCH=1)
    # 辞書は常に esp_mmu_map で貼る (esp_partition_mmap は CONFIG_SPI_FLASH_ROM_IMPL=y だと
    # 8 MB 上限に当たる。上流 M-90 が同じ CoreS3 で esp_mmu_map の動作を確認済み)。
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_DICT_MMU=1)
    # Open JTalk は無改変 (openjtalk/PROVENANCE.md)。IDF の -Werror=all で止まる警告は
    # これらのファイルだけ抑える。`-include oj_heap_psram.h` で calloc/strdup/free を PSRAM
    # 優先に差し替える (絶対パス。set_source_files_properties は上書きなので 1 回にまとめる)。
    set(_saan_k_wno "-Wno-error;-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-implicit-fallthrough;-Wno-unused-function")
    set(_saan_ojt_opts "${_saan_k_wno};-include;${CMAKE_CURRENT_LIST_DIR}/oj_heap_psram.h")
    set_source_files_properties(${_saan_ojt} PROPERTIES COMPILE_OPTIONS "${_saan_ojt_opts}")
    set_source_files_properties(jdict.c accent.c njd_rules.c label_ids.c saan_kanji.c
        PROPERTIES COMPILE_OPTIONS "${_saan_k_wno}")
    message(STATUS "sanoTTS: 端末内漢字 G2P 有効 (dict パーティション 13.7 MB が要る)")
endif()

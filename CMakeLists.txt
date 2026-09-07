cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stackchan_idf)

# --- sanoTTS-jp の重み blob を `sano` パーティションに焼く (CoreS3 / 16 MB 表) ------
# assets/sanotts/saanotts-jp-v3-int8.bin (654,032 B、sanoTTS-jp Model License 1.0 —
# assets/sanotts/NOTICE.md) を raw で書く。app に埋めると 4 MB の OTA スロットを超える
# (実測 3.64 MB + 654 KB) ので、flash_args 経由で `idf.py flash` / pack_firmware.py の
# 一括イメージに載せる。CONFIG_JTTS_ENABLE_SANO=y のボード (cores3) だけ。
if(CONFIG_JTTS_ENABLE_SANO)
    set(SANO_MODEL_BLOB "${CMAKE_CURRENT_LIST_DIR}/assets/sanotts/saanotts-jp-v3-int8.bin")
    if(EXISTS "${SANO_MODEL_BLOB}")
        esptool_py_flash_to_partition(flash "sano" "${SANO_MODEL_BLOB}")
        message(STATUS "sanoTTS: ${SANO_MODEL_BLOB} → sano パーティション (flash 時に書き込み)")
    else()
        message(WARNING "sanoTTS: 重み blob が無い (${SANO_MODEL_BLOB})。sano エンジンはフォールバック動作になる")
    endif()
endif()

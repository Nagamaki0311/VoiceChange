cmake_minimum_required(VERSION 3.25)

project(VoiceChange VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# どのターゲットよりも先に設定する（MSVCの/MT、CMAKE_MSVC_RUNTIME_LIBRARY機能に3.15+が必要）。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.2
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(JUCE)

FetchContent_Declare(
    signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 1.4.0
    GIT_SHALLOW ON
    SYSTEM
)
FetchContent_MakeAvailable(signalsmith-stretch)

# RNNoise v0.2（BSD-3-Clause）。CMakeLists.txtを持たないため、MakeAvailableは取得だけを行う（ビルドは下のvc_rnnoise）。
FetchContent_Declare(
    rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG v0.2
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(rnnoise)

# 学習済みモデル（v0.2のmodel_versionに対応。src/rnnoise_data.c/.hを含む21MBのtar.gz）。Gitには含まれない。
# v0.2の取得スクリプトにはチェックサム検査がないため、SHA256を固定する。
# VC_RNNOISE_MODEL_DOWNLOAD_DIRを指定すると、tar.gzをそこへ保存し、ハッシュが一致すれば再取得しない
# （CIのactions/cacheがこのディレクトリを保存・復元する）。
set(VC_RNNOISE_MODEL_DOWNLOAD_DIR "" CACHE PATH "Directory to keep the RNNoise model tar.gz between builds")
set(VC_RNNOISE_MODEL_DOWNLOAD_OPTIONS "")
if(VC_RNNOISE_MODEL_DOWNLOAD_DIR)
    list(APPEND VC_RNNOISE_MODEL_DOWNLOAD_OPTIONS DOWNLOAD_DIR "${VC_RNNOISE_MODEL_DOWNLOAD_DIR}")
endif()
FetchContent_Declare(
    rnnoise_model
    URL https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz
    URL_HASH SHA256=4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    ${VC_RNNOISE_MODEL_DOWNLOAD_OPTIONS}
)
FetchContent_MakeAvailable(rnnoise_model)

# ===== SECTION: vc_rnnoise (Cの静的ライブラリ。autotoolsは使わない) =====
# ソースはv0.2のsrc/のうちx86/（CPU別最適化のRTCD）とdump_*・write_weights・学習用を除いたもの。rnnoiseのソースは改変しない。
# JUCEを含まないためSTATICでもD-005の懸念（JUCEソースの二重コンパイル）はない。
# CMAKE_MSVC_RUNTIME_LIBRARYは設定済みのため、Cターゲットにも/MTが適用される。
add_library(vc_rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c
    ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    ${rnnoise_SOURCE_DIR}/src/nnet.c
    ${rnnoise_SOURCE_DIR}/src/nnet_default.c
    ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c
    ${rnnoise_model_SOURCE_DIR}/src/rnnoise_data.c
)
target_include_directories(vc_rnnoise SYSTEM PUBLIC ${rnnoise_SOURCE_DIR}/include)
target_include_directories(vc_rnnoise PRIVATE ${rnnoise_SOURCE_DIR}/src ${rnnoise_model_SOURCE_DIR}/src)

if(UNIX)
    target_link_libraries(vc_rnnoise PRIVATE m)
endif()

if(MSVC)
    # OPUS_X86_MAY_HAVE_SSE/SSE2（必須）: v0.2のvec.hは__SSE2__か__AVX__が無いとSIMD無しの経路へ進むが、その経路が
    #   include するos_support.hはv0.2に存在せずコンパイルできない。MSVCは__SSE2__を定義しないため、
    #   x86_arch_macros.h（_MSC_VER節）がこれらから__SSE__/__SSE2__を作り、vec_avx.hのSSE2経路
    #   （GCCのx86_64既定と同じ経路）を有効にする。x86/x64はSSE2が前提。ARM64のWindowsは対象外（NEON経路は__ARM_NEONが必要）。
    # _CRT_SECURE_NO_WARNINGS: fopen等（ファイルからモデルを読む経路。本アプリでは使わない）。
    # M_PIはpitch.cのコメント内だけで、_USE_MATH_DEFINESは不要。
    if(CMAKE_C_COMPILER_ARCHITECTURE_ID MATCHES "ARM" OR CMAKE_VS_PLATFORM_NAME MATCHES "ARM")
        message(FATAL_ERROR "vc_rnnoise: Windows on ARM is not supported (RNNoise v0.2 needs the x86 SSE2 path on MSVC)")
    endif()
    target_compile_definitions(vc_rnnoise PRIVATE
        OPUS_X86_MAY_HAVE_SSE
        OPUS_X86_MAY_HAVE_SSE2
        _CRT_SECURE_NO_WARNINGS
    )
else()
    # -Wno-cpp: nnet.hの「SSE2のみ」の#warning（CPU別最適化を使わない方針のため既知）。
    target_compile_options(vc_rnnoise PRIVATE -Wall -Wno-cpp)
endif()

# ===== SECTION: vc_core (INTERFACE library, D-005) =====
add_library(vc_core INTERFACE)

file(GLOB VC_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp)
target_sources(vc_core INTERFACE ${VC_CORE_SOURCES})

target_include_directories(vc_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(vc_core INTERFACE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_dsp
    signalsmith-stretch
    vc_rnnoise
)

target_compile_definitions(vc_core INTERFACE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_compile_options(vc_core INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/bigobj /utf-8>)

# ===== SECTION: VoiceChange (GUIアプリ) =====
juce_add_gui_app(VoiceChange
    PRODUCT_NAME "VoiceChange"
    COMPANY_NAME "VoiceChange"
    VERSION ${PROJECT_VERSION}
)

file(GLOB VC_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp)
target_sources(VoiceChange PRIVATE ${VC_APP_SOURCES})

target_include_directories(VoiceChange PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(VoiceChange PRIVATE
    JUCE_DIRECTSOUND=0
    JUCE_APPLICATION_NAME_STRING="VoiceChange"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
)

target_link_libraries(VoiceChange PRIVATE
    vc_core
    juce::juce_audio_devices
    juce::juce_gui_extra
    juce::juce_data_structures
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if(WIN32)
    target_link_libraries(VoiceChange PRIVATE psapi)
endif()

# ===== SECTION: VoiceChangeTests =====
juce_add_console_app(VoiceChangeTests
    PRODUCT_NAME "VoiceChangeTests"
)

file(GLOB VC_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
target_sources(VoiceChangeTests PRIVATE ${VC_TEST_SOURCES})

target_include_directories(VoiceChangeTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

# juce_audio_formats: tests/RecordingTool.cppのWAV入出力（T-014。テスト用実行ファイルだけ。アプリ本体にはリンクしない）。
target_link_libraries(VoiceChangeTests PRIVATE
    vc_core
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ===== SECTION: ctest =====
enable_testing()

function(vc_add_test test_name category)
    add_test(NAME ${test_name} COMMAND VoiceChangeTests --category ${category})
    if(${ARGC} GREATER 2 AND "${ARGV2}" STREQUAL "long")
        set_tests_properties(${test_name} PROPERTIES LABELS "long" TIMEOUT 1800)
    else()
        set_tests_properties(${test_name} PROPERTIES LABELS "quick")
    endif()
endfunction()

vc_add_test(smoke Smoke)
vc_add_test(ring_buffer RingBuffer)
vc_add_test(ring_buffer_long RingBufferLong long)
vc_add_test(shifter Shifter)
vc_add_test(engine Engine)
vc_add_test(effects Effects)
vc_add_test(app_logic AppLogic)
vc_add_test(mic Mic)
vc_add_test(long_run LongRun long)

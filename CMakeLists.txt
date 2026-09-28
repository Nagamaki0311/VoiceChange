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

target_link_libraries(VoiceChangeTests PRIVATE
    vc_core
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

cmake_minimum_required(VERSION 3.20)

project(NBA2KAudioFix VERSION 1.0.0 LANGUAGES C)

if(NOT WIN32 OR NOT MINGW)
    message(FATAL_ERROR "NBA2K.AudioFix requires a 32-bit MinGW toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure CMake with an i686 compiler.")
endif()

set(FFMPEG_BUILD_ROOT "" CACHE PATH
    "FFmpeg 8.1.2 i686 static build directory")
set(FFMPEG_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH
    "Matching FFmpeg 8.1.2 source directory")

set(FFMPEG_LIBRARIES
    "${FFMPEG_BUILD_ROOT}/libavformat/libavformat.a"
    "${FFMPEG_BUILD_ROOT}/libavcodec/libavcodec.a"
    "${FFMPEG_BUILD_ROOT}/libswresample/libswresample.a"
    "${FFMPEG_BUILD_ROOT}/libavutil/libavutil.a")

foreach(required_path
        "${FFMPEG_BUILD_ROOT}/config.h"
        "${FFMPEG_SOURCE_ROOT}/libavcodec/avcodec.h"
        ${FFMPEG_LIBRARIES})
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Missing FFmpeg build input: ${required_path}")
    endif()
endforeach()

add_library(NBA2KAudioFix SHARED
    src/audio_fix.c
    src/audio_decoder.c
    src/game_profile.c)

set_target_properties(NBA2KAudioFix PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "NBA2K.AudioFix")

target_include_directories(NBA2KAudioFix PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FFMPEG_BUILD_ROOT}"
    "${FFMPEG_SOURCE_ROOT}")

target_compile_definitions(NBA2KAudioFix PRIVATE
    _WIN32_WINNT=0x0601
    __USE_MINGW_ANSI_STDIO=1
    _CRT_SECURE_NO_WARNINGS)

target_compile_options(NBA2KAudioFix PRIVATE
    -O3
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fdata-sections
    -ffunction-sections
    -fno-ident
    -fno-strict-aliasing
    # Win32 exposes IAT entries through FARPROC.
    -Wno-cast-function-type)

target_link_options(NBA2KAudioFix PRIVATE
    -static
    -static-libgcc
    -Wl,--enable-stdcall-fixup
    -Wl,--exclude-all-symbols
    -Wl,--gc-sections
    -Wl,--no-insert-timestamp
    -Wl,--nxcompat
    -Wl,--dynamicbase)

# The enabled static libav libraries contain circular references.
target_link_libraries(NBA2KAudioFix PRIVATE
    "-Wl,--start-group"
    ${FFMPEG_LIBRARIES}
    "-Wl,--end-group"
    bcrypt
    ole32
    advapi32
    user32
    shell32
    version
    uuid
    ws2_32
    secur32
    atomic
    m)

if(CMAKE_STRIP)
    add_custom_command(TARGET NBA2KAudioFix POST_BUILD
        COMMAND "${CMAKE_STRIP}" --strip-unneeded
            "$<TARGET_FILE:NBA2KAudioFix>"
        VERBATIM)
endif()

add_custom_command(TARGET NBA2KAudioFix POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/NBA2K.AudioFix.ini"
        "$<TARGET_FILE_DIR:NBA2KAudioFix>/NBA2K.AudioFix.ini"
    VERBATIM)

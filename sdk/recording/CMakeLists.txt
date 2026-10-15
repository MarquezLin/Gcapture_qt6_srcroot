add_library(capture_ffmpeg_recorder STATIC
    ffmpeg_video_recorder.cpp
    ffmpeg_video_recorder.h
)

set_target_properties(capture_ffmpeg_recorder PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(capture_ffmpeg_recorder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH
    "FFmpeg root directory")
if (NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()

if (NOT FFMPEG_ROOT)
    message(FATAL_ERROR "capture_ffmpeg_recorder: FFMPEG_ROOT is required.")
endif()

set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
find_library(FFMPEG_AVFORMAT_LIB avformat PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
find_library(FFMPEG_AVCODEC_LIB avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
find_library(FFMPEG_AVUTIL_LIB avutil PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
find_library(FFMPEG_SWSCALE_LIB swscale PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
find_library(FFMPEG_SWRESAMPLE_LIB swresample PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)

if (NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavformat/avformat.h" OR
    NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libswscale/swscale.h" OR
    NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libswresample/swresample.h" OR
    NOT FFMPEG_AVFORMAT_LIB OR NOT FFMPEG_AVCODEC_LIB OR NOT FFMPEG_AVUTIL_LIB OR
    NOT FFMPEG_SWSCALE_LIB OR NOT FFMPEG_SWRESAMPLE_LIB)
    message(FATAL_ERROR
        "capture_ffmpeg_recorder: Required FFmpeg headers/libs were not found under FFMPEG_ROOT: ${FFMPEG_ROOT}")
endif()

target_compile_definitions(capture_ffmpeg_recorder PRIVATE CAPTURE_ENABLE_FFMPEG=1)
target_include_directories(capture_ffmpeg_recorder PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(capture_ffmpeg_recorder PUBLIC
    ${FFMPEG_AVFORMAT_LIB}
    ${FFMPEG_AVCODEC_LIB}
    ${FFMPEG_AVUTIL_LIB}
    ${FFMPEG_SWSCALE_LIB}
    ${FFMPEG_SWRESAMPLE_LIB}
)

if (NOT EXISTS "${FFMPEG_ROOT}/bin/ffmpeg.exe")
    message(FATAL_ERROR
        "capture_ffmpeg_recorder: Cannot verify FFmpeg licensing because ${FFMPEG_ROOT}/bin/ffmpeg.exe is missing.")
endif()

execute_process(
    COMMAND "${FFMPEG_ROOT}/bin/ffmpeg.exe" -version
    RESULT_VARIABLE CAPTURE_FFMPEG_VERSION_RESULT
    OUTPUT_VARIABLE CAPTURE_FFMPEG_VERSION_TEXT
    ERROR_VARIABLE CAPTURE_FFMPEG_VERSION_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (NOT CAPTURE_FFMPEG_VERSION_RESULT EQUAL 0)
    message(FATAL_ERROR
        "capture_ffmpeg_recorder: Failed to inspect FFmpeg licensing: ${CAPTURE_FFMPEG_VERSION_ERROR}")
endif()
if (CAPTURE_FFMPEG_VERSION_TEXT MATCHES "--enable-gpl|--enable-nonfree")
    message(FATAL_ERROR
        "capture_ffmpeg_recorder: FFmpeg at ${FFMPEG_ROOT} uses GPL/nonfree options; an LGPL shared build is required.")
endif()

message(STATUS "capture_ffmpeg_recorder: FFmpeg enabled: ${FFMPEG_ROOT}")

if (MSVC)
    target_compile_options(capture_ffmpeg_recorder PRIVATE /utf-8)
endif()

cmake_minimum_required(VERSION 3.24)

# 项目使用 Qt Widgets + FFmpeg + OpenGLWidgets 组合构建桌面播放器。
project(QtFfmpegOpenGLPlayer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 本地依赖路径通过缓存变量暴露出来，便于在不同机器上覆盖。
set(FFMPEG_ROOT "C:/ffmpeg-dev/ffmpeg-8.1-full_build-shared" CACHE PATH "Path to the FFmpeg shared development package")
set(WINDEPLOYQT_EXECUTABLE "C:/Qt/6.10.1/mingw_64/bin/windeployqt.exe" CACHE FILEPATH "Path to windeployqt")

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGLWidgets Multimedia Network)

qt_standard_project_setup()

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    PATHS "${FFMPEG_ROOT}/include"
    REQUIRED
)

function(find_ffmpeg_library out_var)
    # MinGW 下常见库名前带 lib 前缀，这里兼容两种命名方式。
    if(MINGW)
        find_library(${out_var}
            NAMES "lib${ARGV1}" "${ARGV1}"
            PATHS "${FFMPEG_ROOT}/lib"
            NO_DEFAULT_PATH
            REQUIRED
        )
    else()
        find_library(${out_var}
            NAMES "${ARGV1}"
            PATHS "${FFMPEG_ROOT}/lib"
            NO_DEFAULT_PATH
            REQUIRED
        )
    endif()
    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

find_ffmpeg_library(AVCODEC_LIBRARY avcodec)
find_ffmpeg_library(AVFORMAT_LIBRARY avformat)
find_ffmpeg_library(AVUTIL_LIBRARY avutil)
find_ffmpeg_library(SWRESAMPLE_LIBRARY swresample)
find_ffmpeg_library(SWSCALE_LIBRARY swscale)

add_executable(QtFfmpegOpenGLPlayer
    src/main.cpp
    resources.qrc
    src/ui/AnimatedBackgroundWidget.h
    src/ui/AnimatedBackgroundWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/network/WatchPartyManager.h
    src/network/WatchPartyManager.cpp
    src/player/PlaybackTypes.h
    src/player/PlaybackTypes.cpp
    src/player/PlayerController.h
    src/player/PlayerController.cpp
    src/player/PlayerWorker.h
    src/player/PlayerWorker.cpp
    src/player/SharedAudioBuffer.h
    src/player/SharedAudioBuffer.cpp
    src/player/VideoFrameQueue.h
    src/player/VideoFrameQueue.cpp
    src/audio/AudioBufferDevice.h
    src/audio/AudioBufferDevice.cpp
    src/render/VideoWidget.h
    src/render/VideoWidget.cpp
)

target_include_directories(QtFfmpegOpenGLPlayer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(QtFfmpegOpenGLPlayer
    PRIVATE
        Qt6::Widgets
        Qt6::OpenGLWidgets
        Qt6::Multimedia
        Qt6::Network
        ${AVCODEC_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${SWSCALE_LIBRARY}
)

target_compile_definitions(QtFfmpegOpenGLPlayer PRIVATE NOMINMAX)

if(WIN32)
    if(EXISTS "${WINDEPLOYQT_EXECUTABLE}")
        # 构建完成后自动部署 Qt 运行库与 FFmpeg DLL，方便直接运行打包目录。
        add_custom_command(TARGET QtFfmpegOpenGLPlayer POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:QtFfmpegOpenGLPlayer>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:QtFfmpegOpenGLPlayer>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "C:/Qt/Tools/mingw1310_64/bin/libgcc_s_seh-1.dll"
                "C:/Qt/Tools/mingw1310_64/bin/libstdc++-6.dll"
                "C:/Qt/Tools/mingw1310_64/bin/libwinpthread-1.dll"
                "$<TARGET_FILE_DIR:QtFfmpegOpenGLPlayer>"
            VERBATIM
        )
    else()
        # 若没配置 windeployqt，至少仍拷贝 FFmpeg 与 MinGW 运行时依赖。
        add_custom_command(TARGET QtFfmpegOpenGLPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:QtFfmpegOpenGLPlayer>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "C:/Qt/Tools/mingw1310_64/bin/libgcc_s_seh-1.dll"
                "C:/Qt/Tools/mingw1310_64/bin/libstdc++-6.dll"
                "C:/Qt/Tools/mingw1310_64/bin/libwinpthread-1.dll"
                "$<TARGET_FILE_DIR:QtFfmpegOpenGLPlayer>"
            VERBATIM
        )
    endif()
endif()

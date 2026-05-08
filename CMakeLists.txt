cmake_minimum_required(VERSION 3.22)

project(DawCast VERSION 0.1.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SonarQube用のコンパイルコマンド出力
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS Apple Silicon 専用（arm64 のみ、最低 macOS 12.3 Monterey）
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build arm64 only")
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.3" CACHE STRING "Minimum macOS version (ScreenCaptureKit requires 12.3+)")

# JUCEのパスを指定（CI では -DJUCE_PATH=... で上書き）
set(JUCE_PATH "/Volumes/AUDITIVE/development/JUCE" CACHE PATH "Path to JUCE")
add_subdirectory(${JUCE_PATH} JUCE)

# 配布ビルド設定: FFmpeg dylib を埋め込み + Hardened Runtime + entitlements
# 開発時はデフォルト OFF（Hardened Runtime + ad-hoc 署名 + 外部 dylib の組み合わせは
# library validation で起動失敗するため。また毎ビルドの再署名は TCC をリセットするため）。
# CI/配布ビルドでは -DDAWCAST_EMBED_FFMPEG=ON を指定する。
option(DAWCAST_EMBED_FFMPEG "Embed FFmpeg dylibs and enable Hardened Runtime for distribution" OFF)

if(DAWCAST_EMBED_FFMPEG)
    set(DAWCAST_HARDENED_RUNTIME TRUE)
else()
    set(DAWCAST_HARDENED_RUNTIME FALSE)
endif()

# ─── VST プラグイン（DawCastPlugin） ──────────────────────────────

juce_add_plugin(DawCastPlugin
    COMPANY_NAME "Auditive"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE AudI
    PLUGIN_CODE DwCt
    BUNDLE_ID "com.auditive.dawcast"
    FORMATS AU VST3
    PRODUCT_NAME "DawCast"
    HARDENED_RUNTIME_ENABLED ${DAWCAST_HARDENED_RUNTIME}
)

target_sources(DawCastPlugin
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/DSP/AudioRingBuffer.cpp
        Source/DSP/IPCClient.cpp
        Source/DSP/RecorderAppLauncher.cpp
)

target_compile_definitions(DawCastPlugin
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_include_directories(DawCastPlugin
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(DawCastPlugin
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ─── 録画アプリ（DawCastRecorder） ───────────────────────────────

juce_add_gui_app(DawCastRecorder
    BUNDLE_ID "com.auditive.dawcast.recorder"
    PRODUCT_NAME "DawCastRecorder"
    HARDENED_RUNTIME_ENABLED ${DAWCAST_HARDENED_RUNTIME}
    HARDENED_RUNTIME_OPTIONS "com.apple.security.device.screen-capture"
    PLIST_TO_MERGE "<plist version=\"1.0\"><dict><key>NSScreenCaptureUsageDescription</key><string>DawCastRecorder needs screen recording permission to capture the screen while recording your DAW session.</string><key>LSUIElement</key><true/></dict></plist>"
)

target_sources(DawCastRecorder
    PRIVATE
        Source/Recorder/Main.cpp
        Source/Recorder/RecorderApp.cpp
        Source/Recorder/ScreenRecorder.mm
        Source/Recorder/RegionSelectorWindow.mm
        Source/Recorder/RecordingBorderWindow.mm
        Source/Recorder/AudioReceiver.cpp
        Source/Recorder/FFmpegMuxer.cpp
        Source/Recorder/IPCServer.cpp
        Source/Recorder/OutputManager.cpp
)

target_compile_definitions(DawCastRecorder
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_include_directories(DawCastRecorder
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

# FFmpeg のヘッダ・ライブラリパス（Homebrew デフォルト、CI では上書き可）
set(FFMPEG_PREFIX "/opt/homebrew/opt/ffmpeg" CACHE PATH "Path to FFmpeg installation")

target_include_directories(DawCastRecorder
    PRIVATE
        ${FFMPEG_PREFIX}/include
)

# ScreenRecorder.mm は ARC（自動参照カウント）が必要
set_source_files_properties(Source/Recorder/ScreenRecorder.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
set_source_files_properties(Source/Recorder/RegionSelectorWindow.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
set_source_files_properties(Source/Recorder/RecordingBorderWindow.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_link_libraries(DawCastRecorder
    PRIVATE
        juce::juce_audio_utils
        "-framework ScreenCaptureKit"
        "-framework VideoToolbox"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Metal"
        "${FFMPEG_PREFIX}/lib/libavcodec.dylib"
        "${FFMPEG_PREFIX}/lib/libavformat.dylib"
        "${FFMPEG_PREFIX}/lib/libavutil.dylib"
        "${FFMPEG_PREFIX}/lib/libswresample.dylib"
        "${FFMPEG_PREFIX}/lib/libswscale.dylib"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ─── パッケージング: DawCastRecorder.app を VST3 バンドルに同梱 ─

# ─── FFmpeg dylib を DawCastRecorder.app/Contents/Frameworks/ に埋め込む ─
# 配布時のみ実行（option DAWCAST_EMBED_FFMPEG）。
# 開発時は Homebrew の絶対パス dylib を直接ロードする。
if(DAWCAST_EMBED_FFMPEG)
    add_custom_command(TARGET DawCastRecorder POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:DawCastRecorder>/Contents/Frameworks"
        COMMAND "${CMAKE_SOURCE_DIR}/cmake/embed_ffmpeg.sh"
            "${FFMPEG_PREFIX}/lib"
            "$<TARGET_BUNDLE_DIR:DawCastRecorder>/Contents/Frameworks"
            "$<TARGET_FILE:DawCastRecorder>"
        # install_name_tool で署名が無効化されるため再署名（Hardened Runtime + entitlements を保持）
        COMMAND /usr/bin/codesign --force --sign -
            --options runtime
            --entitlements "${CMAKE_CURRENT_BINARY_DIR}/DawCastRecorder_artefacts/JuceLibraryCode/DawCastRecorder.entitlements"
            --deep
            "$<TARGET_BUNDLE_DIR:DawCastRecorder>"
        COMMENT "Embedding FFmpeg dylibs and re-signing DawCastRecorder.app"
        VERBATIM
    )
endif()

# ─── 開発時署名: 自己署名証明書 "DawCast Dev" を使用 ─────────────────────────
# ad-hoc 署名（--sign -）はビルドごとに CDHash が変わり TCC が毎回ダイアログを出す。
# 自己署名証明書なら TCC が「証明書アンカー + Bundle ID」で追跡するため
# コードを変更してリビルドしてもダイアログが再表示されない。
# 配布ビルド（DAWCAST_EMBED_FFMPEG=ON）は CI で別途署名するため影響なし。
if(NOT DAWCAST_EMBED_FFMPEG)
    # ビルド時は ad-hoc 署名（パスワード不要）。
    # TCC が必要な "DawCast Dev" 署名は make install 時に実施する。
    set_target_properties(DawCastRecorder PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    )
endif()

# VST3 ビルド前に DawCastRecorder のビルドを保証する
add_dependencies(DawCastPlugin_VST3 DawCastRecorder)
add_dependencies(DawCastPlugin_AU DawCastRecorder)

add_custom_command(TARGET DawCastPlugin_VST3 POST_BUILD
    # DawCastRecorder.app を VST3 ビルドアーティファクトに同梱する。
    # make install がここからコピーするため、インストール・署名は行わない。
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DawCastPlugin_VST3>/../Resources/DawCastRecorder.app"
    COMMAND /usr/bin/rsync -a --delete --checksum
        "$<TARGET_BUNDLE_DIR:DawCastRecorder>/"
        "$<TARGET_FILE_DIR:DawCastPlugin_VST3>/../Resources/DawCastRecorder.app/"
    COMMENT "Embedding DawCastRecorder.app into VST3 build artifact"
    VERBATIM
)

# ─── パッケージング: DawCastRecorder.app を AU バンドルに同梱 ────
add_custom_command(TARGET DawCastPlugin_AU POST_BUILD
    # DawCastRecorder.app を AU ビルドアーティファクトに同梱する。
    # make install がここからコピーするため、インストール・署名は行わない。
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DawCastPlugin_AU>/../Resources/DawCastRecorder.app"
    COMMAND /usr/bin/rsync -a --delete --checksum
        "$<TARGET_BUNDLE_DIR:DawCastRecorder>/"
        "$<TARGET_FILE_DIR:DawCastPlugin_AU>/../Resources/DawCastRecorder.app/"
    COMMENT "Embedding DawCastRecorder.app into AU build artifact"
    VERBATIM
)

# ─── Unit Tests ───────────────────────────────────────────────

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

enable_testing()
include(CTest)

juce_add_console_app(DawCastTests PRODUCT_NAME "DawCastTests")
target_sources(DawCastTests PRIVATE
    Source/DSP/AudioRingBuffer.cpp
    Source/DSP/IPCClient.cpp
    Source/DSP/RecorderAppLauncher.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    # Tests/TestAudioRingBuffer.cpp  # 追加予定
)
target_compile_definitions(DawCastTests PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JucePlugin_Name="DawCast"
    JucePlugin_VersionString="0.1.1"
)
target_include_directories(DawCastTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
)
target_link_libraries(DawCastTests
    PRIVATE
        Catch2::Catch2WithMain
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
)

# ── Coverage instrumentation (llvm source-based) ──
target_compile_options(DawCastTests PRIVATE
    -fprofile-instr-generate -fcoverage-mapping)
target_link_options(DawCastTests PRIVATE
    -fprofile-instr-generate -fcoverage-mapping)

include(Catch)
catch_discover_tests(DawCastTests DISCOVERY_MODE PRE_TEST)

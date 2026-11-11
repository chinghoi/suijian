cmake_minimum_required(VERSION 3.20)
project(DJIEditor LANGUAGES CXX)

if (APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Multimedia Network)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_COREML OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "Portable release CPU baseline" FORCE)
foreach(feature SSE42 AVX AVX2 AVX_VNNI BMI2 AVX512 AVX512_VBMI AVX512_VNNI AVX512_BF16 FMA F16C AMX_TILE AMX_INT8 AMX_BF16)
    set(GGML_${feature} OFF CACHE BOOL "Portable x86-64 baseline" FORCE)
endforeach()
FetchContent_Declare(whisper_cpp
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG v1.7.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(whisper_cpp)

qt_standard_project_setup()

qt_add_executable(DJIEditor
    src/main.cpp
    src/MediaEngine.cpp
    src/MediaEngine.h
    src/SpeechTranscriber.cpp
    src/SpeechTranscriber.h
    src/ColorPipeline.cpp
    src/ColorPipeline.h
)

qt_add_qml_module(DJIEditor
    URI DJI.Editor
    VERSION 1.0
    QML_FILES
        qml/AppWindow.qml
        qml/TextBadge.qml
)

target_link_libraries(DJIEditor PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    Qt6::Network
    whisper
)

include(CTest)
if (BUILD_TESTING)
    add_executable(text_badge_test tests/TextBadgeTest.cpp)
    target_link_libraries(text_badge_test PRIVATE Qt6::Quick)
    add_test(NAME text_badge COMMAND text_badge_test "${CMAKE_CURRENT_SOURCE_DIR}/qml")
    set_tests_properties(text_badge PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software" TIMEOUT 15)
    add_executable(qml_window_test tests/QmlWindowTest.cpp)
    target_link_libraries(qml_window_test PRIVATE Qt6::Quick Qt6::QuickControls2)
    add_test(NAME qml_window COMMAND qml_window_test "${CMAKE_CURRENT_SOURCE_DIR}/qml/AppWindow.qml")
    set_tests_properties(qml_window PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software" TIMEOUT 20)
    find_program(NODE_EXECUTABLE node)
    if (NODE_EXECUTABLE)
        add_test(NAME qml_logic COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/QmlLogicTest.cjs)
    endif()
    add_executable(editing_algorithms_test tests/EditingAlgorithmsTest.cpp)
    target_include_directories(editing_algorithms_test PRIVATE src)
    add_test(NAME editing_algorithms COMMAND editing_algorithms_test)
    qt_add_executable(speech_transcriber_test tests/SpeechTranscriberTest.cpp src/SpeechTranscriber.cpp)
    target_include_directories(speech_transcriber_test PRIVATE src)
    target_link_libraries(speech_transcriber_test PRIVATE Qt6::Core Qt6::Network whisper)
    set_target_properties(speech_transcriber_test PROPERTIES MACOSX_BUNDLE FALSE)
    add_test(NAME speech_transcriber COMMAND speech_transcriber_test)
    set_tests_properties(speech_transcriber PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    add_executable(visual_screening_test tests/VisualScreeningTest.cpp)
    target_include_directories(visual_screening_test PRIVATE src)
    add_test(NAME visual_screening COMMAND visual_screening_test)
    qt_add_executable(screening_integration_test
        tests/ScreeningIntegrationTest.cpp
        src/MediaEngine.cpp src/MediaEngine.h
        src/ColorPipeline.cpp src/ColorPipeline.h
        src/SpeechTranscriber.cpp src/SpeechTranscriber.h)
    target_include_directories(screening_integration_test PRIVATE src)
    target_link_libraries(screening_integration_test PRIVATE Qt6::Core Qt6::Gui Qt6::Network whisper)
    set_target_properties(screening_integration_test PROPERTIES MACOSX_BUNDLE FALSE)
    add_test(NAME screening_integration COMMAND screening_integration_test)
    set_tests_properties(screening_integration PROPERTIES TIMEOUT 180)
    qt_add_executable(color_pipeline_test tests/ColorPipelineTest.cpp src/ColorPipeline.cpp)
    target_include_directories(color_pipeline_test PRIVATE src)
    target_link_libraries(color_pipeline_test PRIVATE Qt6::Core)
    set_target_properties(color_pipeline_test PROPERTIES MACOSX_BUNDLE FALSE)
    add_test(NAME color_pipeline COMMAND color_pipeline_test)
    set_tests_properties(color_pipeline PROPERTIES TIMEOUT 120)
endif()

if (APPLE)
    target_sources(DJIEditor PRIVATE src/MacWindowStyle.mm)
    target_link_libraries(DJIEditor PRIVATE "-framework AppKit")
    set_target_properties(DJIEditor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "随剪"
        OUTPUT_NAME "随剪"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
    )
    set_source_files_properties(assets/icon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(DJIEditor PRIVATE assets/icon.icns)
endif()

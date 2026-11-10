cmake_minimum_required(VERSION 3.20)
project(DJIEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Multimedia)

qt_standard_project_setup()

qt_add_executable(DJIEditor
    src/main.cpp
    src/MediaEngine.cpp
    src/MediaEngine.h
    src/ColorPipeline.cpp
    src/ColorPipeline.h
)

qt_add_qml_module(DJIEditor
    URI DJI.Editor
    VERSION 1.0
    QML_FILES
        qml/AppWindow.qml
)

target_link_libraries(DJIEditor PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
)

include(CTest)
if (BUILD_TESTING)
    add_executable(visual_screening_test tests/VisualScreeningTest.cpp)
    target_include_directories(visual_screening_test PRIVATE src)
    add_test(NAME visual_screening COMMAND visual_screening_test)
    qt_add_executable(screening_integration_test
        tests/ScreeningIntegrationTest.cpp src/MediaEngine.cpp src/MediaEngine.h src/ColorPipeline.cpp)
    target_include_directories(screening_integration_test PRIVATE src)
    target_link_libraries(screening_integration_test PRIVATE Qt6::Core Qt6::Gui)
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

cmake_minimum_required(VERSION 3.20)
project(DJIEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Multimedia)

qt_standard_project_setup()

qt_add_executable(DJIEditor
    src/main.cpp
    src/MediaEngine.cpp
    src/MediaEngine.h
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

if (APPLE)
    set_target_properties(DJIEditor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "DJI智能剪辑大师"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
    )
    set_source_files_properties(assets/icon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(DJIEditor PRIVATE assets/icon.icns)
endif()

cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Shared host/module runtime")
set(NEOSHARED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../neoshared" CACHE PATH "NeoShared source directory")
include("${NEOSHARED_ROOT}/cmake/NeoVersion.cmake")
neo_read_version_header(NEOGUI_VERSION
    "${CMAKE_CURRENT_LIST_DIR}/src/core/Version.hpp"
    NEOGUI_VERSION_STRING)
project(NeoGUI VERSION ${NEOGUI_VERSION} LANGUAGES CXX)
include(GNUInstallDirs)
set(NEOSHARED_ENABLE_ERF ON CACHE BOOL "Game archive resources required by NeoGUI" FORCE)
if(NOT TARGET neoshared::gff_app)
    add_subdirectory("${NEOSHARED_ROOT}" "neoshared" EXCLUDE_FROM_ALL)
endif()
include("${NEOSHARED_ROOT}/cmake/NeoMacOSBundle.cmake")
option(NEOGUI_BUILD_WX_GUI "Build the standalone NeoGUI application" ON)
option(NEOGUI_BUILD_WX_MODULE "Build the reusable NeoGUI panel" ON)
if(NEOGUI_BUILD_WX_GUI OR NEOGUI_BUILD_WX_MODULE)
    if(WIN32)
        find_package(wxWidgets CONFIG REQUIRED)
        set(NEOGUI_WX_LIBRARIES wx::core wx::base wx::aui)
    else()
        find_package(wxWidgets REQUIRED COMPONENTS core base aui)
        include(${wxWidgets_USE_FILE})
        set(NEOGUI_WX_LIBRARIES ${wxWidgets_LIBRARIES})
    endif()
    add_library(neogui_wx STATIC src/GuiRenderer.cpp src/GameResources.cpp src/wx/GUIEditorPanel.cpp)
    add_library(neogui::wx ALIAS neogui_wx)
    target_include_directories(neogui_wx PUBLIC src ${wxWidgets_INCLUDE_DIRS})
    target_compile_definitions(neogui_wx PUBLIC ${wxWidgets_DEFINITIONS} NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_features(neogui_wx PUBLIC cxx_std_17)
    target_link_libraries(neogui_wx PUBLIC neoshared::gff_app neoshared::tslpatcher neoshared::texture neoshared::material neoshared::bif neoshared::game neoshared::twoda neoshared::wx ${NEOGUI_WX_LIBRARIES})
    if(MSVC)
        target_compile_options(neogui_wx PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(neogui_wx PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(NEOGUI_BUILD_WX_GUI)
        if(WIN32)
            enable_language(RC)
            add_executable(NeoGUI WIN32 src/wx/main.cpp)
        elseif(APPLE)
            add_executable(NeoGUI MACOSX_BUNDLE src/wx/main.cpp)
        else()
            add_executable(NeoGUI src/wx/main.cpp)
        endif()
        target_link_libraries(NeoGUI PRIVATE neogui::wx)
        neo_add_windows_version_resource(NeoGUI
            VERSION "${PROJECT_VERSION}"
            PRODUCT_NAME "NeoGUI"
            FILE_DESCRIPTION "BioWare GUI layout editor"
            INTERNAL_NAME "NeoGUI"
            ORIGINAL_FILENAME "NeoGUI.exe"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neogui.ico")
        neo_configure_macos_bundle(NeoGUI
            NAME "NeoGUI"
            IDENTIFIER "com.vrifftech.neogui"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neogui.icns"
            VERSION "${PROJECT_VERSION}")
        install(TARGETS NeoGUI BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        if(UNIX AND NOT APPLE)
            foreach(NEO_ICON_SIZE 16 24 32 48 64 128 256 512)
                install(FILES resources/icons/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/neogui.png
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/apps)
            endforeach()
            install(FILES resources/neogui.svg
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
            configure_file(resources/neogui.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/neogui.desktop @ONLY)
            install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neogui.desktop
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
        endif()
    endif()
endif()
neo_apply_minimal_release_to_directory_targets()

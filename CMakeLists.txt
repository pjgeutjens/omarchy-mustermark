cmake_minimum_required(VERSION 3.21)

project(mustermark VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 WebEngineQuick)
if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)
endif()

set(CMARK_GFM_ROOT "$ENV{CMARK_GFM_ROOT}" CACHE PATH "Prefix containing cmark-gfm")
find_path(CMARK_GFM_INCLUDE_DIR cmark-gfm.h
    HINTS "${CMARK_GFM_ROOT}/include" "${CMARK_GFM_ROOT}/usr/include")
find_library(CMARK_GFM_LIBRARY cmark-gfm
    HINTS "${CMARK_GFM_ROOT}/lib" "${CMARK_GFM_ROOT}/usr/lib")
find_library(CMARK_GFM_EXTENSIONS_LIBRARY cmark-gfm-extensions
    HINTS "${CMARK_GFM_ROOT}/lib" "${CMARK_GFM_ROOT}/usr/lib")

if(NOT CMARK_GFM_INCLUDE_DIR OR NOT CMARK_GFM_LIBRARY OR NOT CMARK_GFM_EXTENSIONS_LIBRARY)
    message(FATAL_ERROR "cmark-gfm was not found. Install cmark-gfm or set CMARK_GFM_ROOT.")
endif()

add_library(mustermark_core STATIC
    src/documentsession.cpp
    src/documentsession.h
    src/documentengine.cpp
    src/documentengine.h
    src/filedocument.cpp
    src/linkedfile.cpp
    src/filedocument.h
)
target_include_directories(mustermark_core PUBLIC src PRIVATE "${CMARK_GFM_INCLUDE_DIR}")
target_link_libraries(mustermark_core
    PUBLIC Qt6::Core
    PRIVATE "${CMARK_GFM_LIBRARY}" "${CMARK_GFM_EXTENSIONS_LIBRARY}")
target_compile_options(mustermark_core PRIVATE -Wall -Wextra -Wpedantic)

qt_add_executable(mustermark
    src/main.cpp
    src/documentcontroller.cpp
    src/documentcontroller.h
    src/markdownhighlighter.cpp
    src/markdownhighlighter.h
    src/previewipc.cpp
    src/previewipc.h
    src/cli.cpp
    src/cli.h
    src/httpserver.cpp
    src/httpserver.h
    resources.qrc
)
target_link_libraries(mustermark PRIVATE
    mustermark_core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2
    Qt6::WebEngineQuick)

install(TARGETS mustermark RUNTIME DESTINATION bin)
install(FILES packaging/mustermark.desktop DESTINATION share/applications)
install(FILES packaging/mustermark.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION share/licenses/mustermark)

if(BUILD_TESTING)
    add_executable(mustermark_tests tests/test_documentengine.cpp)
    target_link_libraries(mustermark_tests PRIVATE mustermark_core Qt6::Test)
    add_test(NAME mustermark_tests COMMAND mustermark_tests)

    qt_add_executable(mustermark_qml_tests
        tests/test_qml.cpp
        src/httpserver.cpp
        src/httpserver.h
        src/documentcontroller.cpp
        src/documentcontroller.h
        src/markdownhighlighter.cpp
        src/markdownhighlighter.h
        src/previewipc.cpp
        src/previewipc.h
        resources.qrc
    )
    target_link_libraries(mustermark_qml_tests PRIVATE
        mustermark_core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test
        Qt6::WebEngineQuick)
    add_test(NAME mustermark_qml_tests COMMAND mustermark_qml_tests)
    set_tests_properties(mustermark_qml_tests PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=generic;QT_QUICK_BACKEND=software;QT_STYLE_OVERRIDE=Basic;XDG_DATA_HOME=/tmp/mustermark-test-data")

    qt_add_executable(mustermark_demo
        demo/runner.cpp src/documentcontroller.cpp src/markdownhighlighter.cpp
        src/previewipc.cpp src/httpserver.cpp resources.qrc)
    target_include_directories(mustermark_demo PRIVATE src)
    target_compile_definitions(mustermark_demo PRIVATE DEMO_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(mustermark_demo PRIVATE mustermark_core Qt6::Gui Qt6::Network
        Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::WebEngineQuick Qt6::Test)

    find_program(CURL_EXECUTABLE curl)
    find_program(JQ_EXECUTABLE jq)
    find_program(NODE_EXECUTABLE node)
    find_program(CHROMIUM_EXECUTABLE chromium)
    if(CURL_EXECUTABLE AND JQ_EXECUTABLE)
        add_test(NAME mustermark_http_tests
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_http_serving.sh
                    $<TARGET_FILE:mustermark> ${CMAKE_SOURCE_DIR})
        add_test(NAME mustermark_stdio_tests
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_stdio_api.sh
                    $<TARGET_FILE:mustermark> ${CMAKE_SOURCE_DIR})
        add_test(NAME mustermark_shared_gui_tests
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_shared_gui_server.sh
                    $<TARGET_FILE:mustermark> ${CMAKE_SOURCE_DIR})
        add_test(NAME mustermark_preview_app_tests
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_preview_app.sh
                    $<TARGET_FILE:mustermark> ${CMAKE_SOURCE_DIR})
        if(NODE_EXECUTABLE AND CHROMIUM_EXECUTABLE)
            add_test(NAME mustermark_visual_draft_browser_tests
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_visual_draft_browser.sh
                        $<TARGET_FILE:mustermark> ${CMAKE_SOURCE_DIR})
        endif()
    endif()
endif()

cmake_minimum_required(VERSION 3.21)

# Qt 6.10's frameworks are built for macOS 13; without this the app would
# require the macOS version of the machine that built it. (CMake pre-creates
# the variable as empty.)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)
endif()

project(rust-commander VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent Svg)
qt_standard_project_setup()

# Everything except main() goes into a library, so the tests can use it too.
add_library(app_core STATIC
    src/app/Dialogs.cpp
    src/app/Dialogs.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/Settings.cpp
    src/app/Settings.h
    src/app/Theme.cpp
    src/app/Theme.h
    src/panels/FileModel.cpp
    src/panels/FileModel.h
    src/panels/FileProxyModel.cpp
    src/panels/FileProxyModel.h
    src/panels/FileView.cpp
    src/panels/FileView.h
    src/panels/PathBar.cpp
    src/panels/PathBar.h
    src/ops/FileOps.cpp
    src/ops/FileOps.h
    src/ops/OperationRunner.cpp
    src/ops/OperationRunner.h
    src/ops/Shell.cpp
    src/ops/Shell.h
)
target_link_libraries(app_core PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::Svg)
target_include_directories(app_core PUBLIC src)
target_compile_definitions(app_core PUBLIC APP_VERSION="${PROJECT_VERSION}")

if(WIN32)
    # No console window behind the app on Windows.
    qt_add_executable(rust-commander WIN32 src/main.cpp)
else()
    qt_add_executable(rust-commander src/main.cpp)
endif()
target_link_libraries(rust-commander PRIVATE app_core)

if(APPLE)
    # The app icon, generated from the PNG in resources/ at configure time.
    set(iconset "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset")
    file(MAKE_DIRECTORY "${iconset}")
    foreach(size 16 32 128 256 512)
        math(EXPR double "${size} * 2")
        execute_process(
            COMMAND sips -s format png -z ${size} ${size}
                    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
                    --out "${iconset}/icon_${size}x${size}.png"
            OUTPUT_QUIET ERROR_QUIET)
        execute_process(
            COMMAND sips -s format png -z ${double} ${double}
                    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png"
                    --out "${iconset}/icon_${size}x${size}@2x.png"
            OUTPUT_QUIET ERROR_QUIET)
    endforeach()
    execute_process(
        COMMAND iconutil -c icns "${iconset}" -o "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns"
        OUTPUT_QUIET ERROR_QUIET)

    set_target_properties(rust-commander PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist")
    target_sources(rust-commander PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
endif()

# The tests run on the "offscreen" platform, so they need no display.
include(CTest)
# A test that hangs must not hold up a build.
set(CTEST_TEST_TIMEOUT 60)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    set(APP_TESTS filemodel fileview fileops shell operationrunner pathbar)
    foreach(test IN LISTS APP_TESTS)
        add_executable(test_${test} tests/${test}.cpp)
        target_link_libraries(test_${test} PRIVATE app_core Qt6::Test)
        add_test(NAME ${test} COMMAND test_${test})
        set_tests_properties(${test} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()

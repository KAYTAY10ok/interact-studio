cmake_minimum_required(VERSION 3.18)
set(version_file "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${version_file}")
file(READ "${version_file}" studio_version)
string(STRIP "${studio_version}" studio_version)
if(NOT studio_version MATCHES [[^[0-9]+\.[0-9]+\.[0-9]+$]])
    message(FATAL_ERROR "VERSION must contain a version of the form MAJOR.MINOR.PATCH")
endif()
project(InteractStudio VERSION "${studio_version}" LANGUAGES CXX)

include(CTest)
find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Network)

set(studio_sources
    src/buildrun.cpp
    src/catalog.cpp
    src/customize.cpp
    src/definition.cpp
    src/editor.cpp
    src/formatting.cpp
    src/icons.cpp
    src/intellisense.cpp
    src/languages.cpp
    src/liveserver.cpp
    src/quickopen.cpp
    src/studio.cpp
    src/workspace.cpp
)

if(WIN32)
    list(APPEND studio_sources src/terminal_win.cpp)
else()
    find_package(qtermwidget5 CONFIG REQUIRED)
    list(APPEND studio_sources src/terminal.cpp)
endif()

add_library(studio_core STATIC ${studio_sources})
target_compile_features(studio_core PUBLIC cxx_std_17)
target_include_directories(studio_core PUBLIC src)
target_link_libraries(studio_core PUBLIC Qt5::Widgets Qt5::Network)
if(NOT WIN32)
    target_link_libraries(studio_core PUBLIC qtermwidget5)
endif()

function(studio_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -finput-charset=UTF-8)
    endif()
endfunction()
studio_warnings(studio_core)

if(WIN32)
    add_executable(interact-studio WIN32 src/main.cpp)
    set_target_properties(interact-studio PROPERTIES OUTPUT_NAME InteractStudio)
else()
    add_executable(interact-studio src/main.cpp)
endif()
target_link_libraries(interact-studio PRIVATE studio_core)
target_compile_definitions(interact-studio PRIVATE INTERACT_STUDIO_VERSION="${PROJECT_VERSION}")
studio_warnings(interact-studio)

if(BUILD_TESTING)
    # Linux additionally exercises the PTY and all modules via `make check`.
    add_executable(studio-desktop-test tests/desktop_test.cpp)
    target_link_libraries(studio-desktop-test PRIVATE studio_core)
    studio_warnings(studio-desktop-test)
    add_test(NAME desktop-launch COMMAND studio-desktop-test)
    set_tests_properties(desktop-launch PROPERTIES TIMEOUT 20)
    foreach(module IN ITEMS catalog editor formatting intellisense languages liveserver workspace_git)
        add_executable(studio-${module}-test tests/${module}_test.cpp)
        target_link_libraries(studio-${module}-test PRIVATE studio_core)
        studio_warnings(studio-${module}-test)
        add_test(NAME ${module} COMMAND studio-${module}-test)
        set_tests_properties(${module} PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    if(WIN32)
        add_executable(studio-terminal-test tests/terminal_win_test.cpp)
        target_link_libraries(studio-terminal-test PRIVATE studio_core)
        studio_warnings(studio-terminal-test)
        add_test(NAME windows-terminal COMMAND studio-terminal-test)
        set_tests_properties(windows-terminal PROPERTIES TIMEOUT 20)
    else()
        set_tests_properties(desktop-launch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

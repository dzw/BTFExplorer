cmake_minimum_required(VERSION 3.20)

project(PagedExplorer LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "PagedExplorer requires Windows and the Win32 Shell APIs.")
endif()

add_executable(PagedExplorer WIN32
    src/App/main.cpp
    src/Shell/ShellContextMenu.cpp
    src/Shell/ShellEnumerator.cpp
    src/Shell/ShellFileOperation.cpp
    src/Shell/ShellUtil.cpp
    src/Pagination/PageManager.cpp
    src/Util/AppLog.cpp
    src/UI/DirectoryTree.cpp
    src/UI/MainWindow.cpp
    src/UI/TrayIcon.cpp
)

target_compile_features(PagedExplorer PRIVATE cxx_std_20)
target_compile_definitions(PagedExplorer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(PagedExplorer PRIVATE
    Comctl32
    Shell32
    Ole32
    OleAut32
    Shlwapi
    Propsys
    User32
    Gdi32
    Advapi32
)

if(MSVC)
    target_compile_options(PagedExplorer PRIVATE /W4 /sdl /utf-8)
endif()

set_target_properties(PagedExplorer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

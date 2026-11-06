cmake_minimum_required(VERSION 3.25)
project(VTabsNotepad VERSION 1.2.0 LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "VTabsNotepad va compilato a 64 bit (x64): il Blocco note è un processo x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# CRT statica: la DLL gira dentro Notepad, che carica la propria versione del VC runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Proiezioni C++/WinRT generate dai .winmd installati (Windows SDK, Windows App SDK, NotepadXamlUI).
set(NPX_CPPWINRT_DIR "${CMAKE_BINARY_DIR}/generated/cppwinrt")
if(NOT EXISTS "${NPX_CPPWINRT_DIR}/winrt/NotepadXamlUI.h")
    message(STATUS "Generazione delle proiezioni C++/WinRT...")
    execute_process(
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tools/Generate-Projections.ps1" -OutDir "${NPX_CPPWINRT_DIR}"
        RESULT_VARIABLE npx_projection_result)
    if(NOT npx_projection_result EQUAL 0)
        message(FATAL_ERROR "Generazione delle proiezioni C++/WinRT fallita (${npx_projection_result}).")
    endif()
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj /EHsc)

# Codice condiviso tra launcher e mod.
add_library(npx_common STATIC
    src/common/Settings.cpp
    src/common/Shared.cpp)
target_include_directories(npx_common PUBLIC src/common)

# VTabsNotepadMod.dll: iniettata nel Blocco note, modifica la sua interfaccia XAML.
add_library(VTabsNotepadMod SHARED
    src/mod/dllmain.cpp
    src/mod/CaptionButtons.cpp
    src/mod/Glass.cpp
    src/mod/Interop.cpp
    src/mod/Log.cpp
    src/mod/ModCore.cpp
    src/mod/TabColors.cpp
    src/mod/Tap.cpp
    src/mod/TreeDump.cpp
    src/mod/VisualTreeWatcher.cpp
    src/mod/WindowLayout.cpp
    src/mod/features/BackToTop.cpp
    src/mod/features/ReopenTab.cpp
    src/mod/features/ScrollBarFit.cpp
    src/mod/features/ScrollBarHover.cpp
    src/mod/features/WheelScroll.cpp
    src/mod/features/ScrollRangeFix.cpp
    src/mod/features/VerticalTabs.cpp
    src/mod/VTabsNotepadMod.def)
target_include_directories(VTabsNotepadMod PRIVATE "${NPX_CPPWINRT_DIR}" src/mod)
target_precompile_headers(VTabsNotepadMod PRIVATE src/mod/pch.h)
target_link_libraries(VTabsNotepadMod PRIVATE npx_common windowsapp comctl32 ole32 oleaut32 user32 shell32 dwmapi uxtheme d2d1 dwrite)

# VTabsNotepad.exe: launcher con icona nella tray.
add_executable(VTabsNotepad WIN32
    src/launcher/main.cpp
    src/launcher/Injector.cpp
    src/launcher/TrayIcon.cpp
    src/launcher/VTabsNotepad.rc
    src/launcher/VTabsNotepad.manifest)
target_link_libraries(VTabsNotepad PRIVATE npx_common user32 shell32 gdi32 gdiplus advapi32 ole32)
add_dependencies(VTabsNotepad VTabsNotepadMod)

# Strumento dei test (tools\test): menu contestuale di un elemento via UI Automation.
add_executable(VTabsNotepadUiaTool tools/test/UiaContextMenu.cpp)
set_target_properties(VTabsNotepadUiaTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
target_link_libraries(VTabsNotepadUiaTool PRIVATE ole32 oleaut32)

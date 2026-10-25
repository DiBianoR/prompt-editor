cmake_minimum_required(VERSION 3.20)
project(PromptEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/utf-8 /MP)

# Scintilla, built as a static library (no DLL entry point, no lexers).
set(SCI_DIR ${CMAKE_SOURCE_DIR}/third_party/scintilla)
file(GLOB SCI_SOURCES ${SCI_DIR}/src/*.cxx ${SCI_DIR}/win32/*.cxx)
list(FILTER SCI_SOURCES EXCLUDE REGEX "ScintillaDLL\\.cxx$")
add_library(scintilla STATIC ${SCI_SOURCES})
target_include_directories(scintilla PUBLIC ${SCI_DIR}/include PRIVATE ${SCI_DIR}/src)
target_compile_definitions(scintilla PRIVATE NO_CXX11_REGEX)
target_compile_options(scintilla PRIVATE /W1 /EHsc)
target_link_libraries(scintilla PUBLIC imm32 msimg32 ole32 oleaut32 uuid advapi32 gdi32 user32)

# Index model, anchoring and edit tracking: no UI, tested headlessly.
add_library(pecore STATIC
    src/core/Index.cpp
    src/core/Anchor.cpp
    src/core/Text.cpp
    src/core/Tracker.cpp)
target_include_directories(pecore PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(pecore PRIVATE /W4 /EHsc)
target_link_libraries(pecore PUBLIC bcrypt)

add_executable(core_tests tests/core_tests.cpp)
target_compile_options(core_tests PRIVATE /W4 /EHsc)
target_link_libraries(core_tests PRIVATE pecore)

enable_testing()
add_test(NAME core_tests COMMAND core_tests ${CMAKE_SOURCE_DIR}/tests/fixtures/local)

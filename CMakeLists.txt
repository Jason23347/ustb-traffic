cmake_minimum_required(VERSION 3.16)
project(UstbTraffic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I${CMAKE_SOURCE_DIR}/src")

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
endif()

set(USTB_CORE
  src/parser.cpp
  src/debounce.cpp
  src/format.cpp
)

add_executable(UstbTraffic WIN32
  ${USTB_CORE}
  src/main.cpp
  src/app.cpp
  src/config.cpp
  src/poller.cpp
  src/taskbar_wnd.cpp
  src/menu.cpp
  src/autostart.cpp
  src/app.rc
)

target_include_directories(UstbTraffic PRIVATE src)
target_compile_definitions(UstbTraffic PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
)
target_link_libraries(UstbTraffic PRIVATE
  user32
  gdi32
  winhttp
  shell32
  advapi32
  comctl32
  ole32
)
if(MSVC)
  target_link_options(UstbTraffic PRIVATE /MANIFEST:NO)
endif()

add_executable(UstbTrafficTests
  ${USTB_CORE}
  tests/test_main.cpp
)
target_include_directories(UstbTrafficTests PRIVATE src)
target_compile_definitions(UstbTrafficTests PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

enable_testing()
add_test(NAME UstbTrafficTests COMMAND UstbTrafficTests)

cmake_minimum_required(VERSION 3.24)
project(WindowsSpeedMaster LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Windows Speed Master is a Windows-only Win32 application.")
endif()

add_executable(WindowsSpeedMaster WIN32 src/main.cpp)
target_compile_features(WindowsSpeedMaster PRIVATE cxx_std_17)
target_compile_definitions(WindowsSpeedMaster PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(WindowsSpeedMaster PRIVATE comctl32 advapi32 shell32 ole32 taskschd comsuppw)

if(MSVC)
  target_compile_options(WindowsSpeedMaster PRIVATE /W4 /permissive- /EHsc)
endif()

install(TARGETS WindowsSpeedMaster RUNTIME DESTINATION .)

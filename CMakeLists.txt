cmake_minimum_required(VERSION 3.24)
project(WinStartReview LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "WinStartReview is a Windows-only Win32 application.")
endif()

add_executable(WinStartReview WIN32 src/main.cpp)
target_compile_features(WinStartReview PRIVATE cxx_std_17)
target_compile_definitions(WinStartReview PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(WinStartReview PRIVATE comctl32 advapi32 shell32 ole32 taskschd comsuppw)

if(MSVC)
  target_compile_options(WinStartReview PRIVATE /W4 /permissive- /EHsc)
endif()

install(TARGETS WinStartReview RUNTIME DESTINATION .)

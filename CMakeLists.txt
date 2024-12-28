cmake_minimum_required(VERSION 3.8)

project(NTop
  LANGUAGES
    C
    CXX
)

option(ENABLE_UNICODE "Compile with Unicode support" OFF)

add_executable(${PROJECT_NAME}
  ntop.c
  util.c
  vi.c
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE
    $<$<BOOL:${ENABLE_UNICODE}>:
      UNICODE
      _UNICODE
    >
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    WindowsApp
    wbemuuid
)

target_compile_features(${PROJECT_NAME}
  PRIVATE cxx_std_17
)

# Temporary fix for C++/WinRT bug
target_compile_options(${PROJECT_NAME}
  PRIVATE /permissive
)

find_package(WIL CONFIG REQUIRED)

add_executable(wmi
  main.c
  wmi.cpp
)

target_link_libraries(wmi
  PRIVATE
    WindowsApp
    wbemuuid
    WIL::WIL
)

target_compile_features(wmi
  PRIVATE
    cxx_std_17
)

# Temporary fix for C++/WinRT bug
target_compile_options(wmi
  PRIVATE /permissive
)

add_executable(test
  main.cpp
)

target_link_libraries(test
  PRIVATE
    WindowsApp
    wbemuuid
    WIL::WIL
)

target_compile_features(test
  PRIVATE
    cxx_std_17
)

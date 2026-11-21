cmake_minimum_required(VERSION 3.15)
project(dsetup LANGUAGES C RC)

# The original dsetup.dll is a 32-bit DLL. Always build x86.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(WARNING "dsetup.dll is a 32-bit module: configure with -A Win32 (MSVC) "
                  "or an i686 toolchain. Building 64-bit changes the ABI.")
endif()

add_library(dsetup SHARED src/dsetup.c src/dsetup.def src/dsetup.rc)
target_include_directories(dsetup PRIVATE include)
set_target_properties(dsetup PROPERTIES PREFIX "" OUTPUT_NAME "dsetup")
target_link_libraries(dsetup PRIVATE advapi32 user32 winmm version ole32 kernel32)

if(MSVC)
  # static CRT like the original (no msvcr*.dll dependency)
  set_property(TARGET dsetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_compile_options(dsetup PRIVATE /W3 /O1 /GS)
  target_compile_definitions(dsetup PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_options(dsetup PRIVATE /SUBSYSTEM:WINDOWS /NXCOMPAT /DYNAMICBASE)
else()
  target_compile_options(dsetup PRIVATE -Wall -Wextra -Os)
  target_link_options(dsetup PRIVATE -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup)
endif()

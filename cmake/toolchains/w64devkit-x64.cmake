set(CMAKE_SYSTEM_NAME Windows)

set(_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.toolchain/w64devkit/w64devkit")
set(CMAKE_C_COMPILER "${_ROOT}/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "${_ROOT}/bin/g++.exe")
set(CMAKE_RC_COMPILER "${_ROOT}/bin/windres.exe")
set(CMAKE_MAKE_PROGRAM "${CMAKE_CURRENT_LIST_DIR}/../../.toolchain/ninja/ninja.exe" CACHE FILEPATH "" FORCE)


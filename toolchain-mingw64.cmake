# =============================================================================
# CMake Toolchain File for MinGW-w64 Cross-Compilation
# Target: Windows x86_64 PE32+
# =============================================================================

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Toolchain prefix
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# Cross compilers and binutils
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}-ar CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB       ${TOOLCHAIN_PREFIX}-ranlib CACHE FILEPATH "Ranlib")
set(CMAKE_STRIP        ${TOOLCHAIN_PREFIX}-strip CACHE FILEPATH "Strip")

# Target search prefix paths
set(CMAKE_FIND_ROOT_PATH
    /usr/${TOOLCHAIN_PREFIX}
    /usr/lib/gcc/${TOOLCHAIN_PREFIX}
)

# Search rules: never search host for programs; search target root for libs/headers
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Strict static linking flags default
set(CMAKE_C_FLAGS_INIT "-Wall -Wextra -static")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")

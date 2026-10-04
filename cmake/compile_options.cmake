set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

# ------------------------------------------------------------
# MSVC compiler
# ------------------------------------------------------------
if (MSVC)
    # https://cmake.org/cmake/help/latest/policy/CMP0091.html
    cmake_policy(SET CMP0091 NEW)

    message(STATUS "Configuring for MSVC compiler")
    message(STATUS "MSVC version: ${MSVC_VERSION}")
    
    add_compile_definitions(    
        _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
        _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS)

    set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo" CACHE STRING "" FORCE)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "^Visual Studio")
        add_compile_options(/MP)
        # Share the compiler process limit across projects; defaults to the logical CPU count.
        list(APPEND CMAKE_VS_GLOBALS
            UseMultiToolTask=true
            EnforceProcessCountAcrossBuilds=true)
    endif()

    add_compile_options(
        /nologo # Suppress compiler startup banner
        /utf-8 # Set source and execution character set to UTF-8
        /permissive- # Enforce standard compliance

        # Debug config
        "$<$<CONFIG:Debug>:/Od>"
        "$<$<CONFIG:Debug>:/MTd>" # Multi-threaded Debug Runtime (static)
        "$<$<CONFIG:Debug>:/Z7>" # Debug symbols are stored in each .obj file. The linker merges them into the final .pdb if requested. No .pdb is created during compilation — only at link time.
        "$<$<CONFIG:Debug>:/FS>" # Prevents simultaneous writes to .pdb (debug symbols) from multiple compiler processes
        "$<$<CONFIG:Debug>:/D_ITERATOR_DEBUG_LEVEL=2>" # # Controls STL iterator safety checks - Full checks

        # Rel with deb info config
        "$<$<CONFIG:RelWithDebInfo>:/O2>" # Maximize Speed Optimization
        "$<$<CONFIG:RelWithDebInfo>:/MT>" # Multi-threaded Runtime (static)
        "$<$<CONFIG:RelWithDebInfo>:/Z7>" # Debug symbols are stored in each .obj file. The linker merges them into the final .pdb if requested. No .pdb is created during compilation — only at link time.
        "$<$<CONFIG:RelWithDebInfo>:/FS>" # Prevents simultaneous writes to .pdb (debug symbols) from multiple compiler processes
        "$<$<CONFIG:RelWithDebInfo>:/D_ITERATOR_DEBUG_LEVEL=0>" # Controls STL iterator safety checks - No checks
        "$<$<CONFIG:RelWithDebInfo>:/DNDEBUG>" # Disables assert macros and other debug checks

        # Release config
        "$<$<CONFIG:Release>:/O2>" # Maximize Speed Optimization
        "$<$<CONFIG:Release>:/MT>" # Multi-threaded Runtime (static)
        "$<$<CONFIG:Release>:/D_ITERATOR_DEBUG_LEVEL=0>" # Controls STL iterator safety checks - No checks
        "$<$<CONFIG:Release>:/DNDEBUG>"  # Disable assert() and other debug macros
        "$<$<CONFIG:Release>:/GL>"       # Enable LTCG (Link-Time Code Generation) for further optimization
        "$<$<CONFIG:Release>:/Oi>"       # Enable intrinsic functions (e.g., __builtin_memcpy)
        "$<$<CONFIG:Release>:/Gy>"       # Function-level linking (better for /OPT:REF, /OPT:ICF)
    )

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

    # Dla 64-bitowych binarek zaleca się adresy powyżej 4 GB (0x100000000), 
    # aby w pełni wykorzystać optymalizacje zabezpieczeń oferowane przez ASLR 
    # (Address Space Layout Randomization)
    # The address 0x100000000 is the bare minimum above 4 GB, but it might still be adjacent to system-allocated memory regions.
    # Windows loader can reserve some areas just above 4 GB for internal use. Placing your binary at 0x140000000 gives a safer padding buffer.
    # 0x140000000 (5.375 GB) is used by Visual Studio by default for 64-bit EXEs.
    # It's less likely to conflict with other DLLs or memory-mapped files in modern Windows processes.
    # It's aligned on a larger boundary (512 MB), which is cleaner and avoids fragmentation.
    
    # Global linker flags
    set(CMAKE_EXE_LINKER_FLAGS "/BASE:0x140000000" CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}" CACHE STRING "" FORCE)

    # Linker flags for Debug
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG
        "/DEBUG /INCREMENTAL /OPT:NOREF /OPT:NOICF"
        CACHE STRING "" FORCE)

    # Linker flags for RelWithDebInfo
    set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
        "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF /LTCG"
        CACHE STRING "" FORCE)

    # Linker flags for Release
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE"
        CACHE STRING "" FORCE)

    # Also apply to shared libraries:
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG           "${CMAKE_EXE_LINKER_FLAGS_DEBUG}"           CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO  "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}"  CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE         "${CMAKE_EXE_LINKER_FLAGS_RELEASE}"         CACHE STRING "" FORCE)

endif()

# ------------------------------------------------------------
# GCC and Clang (including AppleClang) warnings
# ------------------------------------------------------------
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        -Wno-unknown-pragmas
        -fdiagnostics-color=always
    )
endif()

# ------------------------------------------------------------
# GCC compiler
# ------------------------------------------------------------
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "Configuring for GCC compiler")
    message(STATUS "GCC version: ${CMAKE_CXX_COMPILER_VERSION}")

    add_compile_options(
        # Debug config
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Debug>:-g3>"
        "$<$<CONFIG:Debug>:-DDEBUG>"

        # RelWithDebInfo config
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-DNDEBUG>"

        # Release config
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-DNDEBUG>"
    )

    # Linker flags (optional, but good for optimized builds)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-s -Wl,--as-needed -Wl,--gc-sections" CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "" FORCE)
endif()

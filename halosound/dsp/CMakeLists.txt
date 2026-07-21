cmake_minimum_required(VERSION 3.13)
project(halosound_dsp C)

set(CMAKE_C_STANDARD 11)

# Source files
set(DSP_SOURCES
    src/hrtf_engine.c
    src/hrtf_convolver.c
    src/freeverb.c
    src/er_spatial.c
    src/sofa_loader.c
    src/channel_layouts.c
)

set(PFFFT_SOURCES
    pffft/pffft.c
    pffft/pffft_common.c
    pffft/fftpack.c
)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/pffft
)

if(EMSCRIPTEN)
    # =========================================================================
    # Emscripten / WebAssembly build
    # =========================================================================

    add_executable(halosound_dsp ${DSP_SOURCES} ${PFFFT_SOURCES})

    # PFFFT: force scalar fallback (no SIMD in base WASM)
    target_compile_definitions(halosound_dsp PRIVATE
        PFFFT_SCALVEC_ENABLED=1
    )

    # Optimization
    target_compile_options(halosound_dsp PRIVATE -O3 -ffast-math)

    # Emscripten link flags
    set(EXPORTED_FUNCS "[\"_halo_create\",\"_halo_destroy\",\"_halo_load_sofa\",\"_halo_set_layout\",\"_halo_set_speaker_pos\",\"_halo_set_room\",\"_halo_set_room_preset\",\"_halo_process\",\"_halo_get_num_channels\",\"_malloc\",\"_free\"]")
    set(EXPORTED_RT "[\"ccall\",\"cwrap\",\"HEAPF32\",\"HEAPU8\"]")

    target_link_options(halosound_dsp PRIVATE
        -sWASM=1
        -sMODULARIZE=1
        -sEXPORT_NAME=HaloSoundDSP
        "-sEXPORTED_FUNCTIONS=${EXPORTED_FUNCS}"
        "-sEXPORTED_RUNTIME_METHODS=${EXPORTED_RT}"
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=16777216
        -sMAXIMUM_MEMORY=67108864
        -sNO_EXIT_RUNTIME=1
        "-sENVIRONMENT=web,worker"
        -sFILESYSTEM=0
        -sASSERTIONS=0
        -O3
    )
    set_target_properties(halosound_dsp PROPERTIES SUFFIX ".js")

    # Copy outputs to build directory
    add_custom_command(TARGET halosound_dsp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:halosound_dsp>/halosound_dsp.wasm
            ${CMAKE_CURRENT_SOURCE_DIR}/build/halosound_dsp.wasm
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:halosound_dsp>/halosound_dsp.js
            ${CMAKE_CURRENT_SOURCE_DIR}/build/halosound_dsp.js
        COMMENT "Copying WASM artifacts to build/"
    )

else()
    # =========================================================================
    # Native build (for testing on PC)
    # =========================================================================

    # Build as static library
    add_library(halosound_dsp STATIC ${DSP_SOURCES} ${PFFFT_SOURCES})

    target_compile_options(halosound_dsp PRIVATE -O2)

    # Check for libmysofa
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MYSOFA QUIET libmysofa)
        if(MYSOFA_FOUND)
            target_compile_definitions(halosound_dsp PRIVATE HAVE_MYSOFA)
            target_include_directories(halosound_dsp PRIVATE ${MYSOFA_INCLUDE_DIRS})
            target_link_libraries(halosound_dsp ${MYSOFA_LIBRARIES})
        endif()
    endif()

    # Math library (Linux)
    target_link_libraries(halosound_dsp m)

    # Simple test executable
    add_executable(halosound_test test/test_dsp.c)
    target_link_libraries(halosound_test halosound_dsp m)

endif()

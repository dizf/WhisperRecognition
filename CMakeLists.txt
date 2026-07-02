cmake_minimum_required(VERSION 3.15)
project(WhisperWhisperPOC CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Setup thirdparty directory in the project source tree
set(THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/thirdparty")
file(MAKE_DIRECTORY "${THIRDPARTY_DIR}")
file(MAKE_DIRECTORY "${THIRDPARTY_DIR}/models")

# Configure output directories to be placed in bin/ at the project root
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

# 1. Setup whisper.cpp from local zip inside thirdparty, otherwise download
set(WHISPER_ZIP_PATH "${THIRDPARTY_DIR}/v1.9.1.zip")
set(WHISPER_SRC_DIR "${THIRDPARTY_DIR}/whisper.cpp")

if(NOT EXISTS "${WHISPER_SRC_DIR}")
    # Check if we have the cache zip from the build folder
    if(EXISTS "${CMAKE_SOURCE_DIR}/build_vs/v1.9.1.zip" AND NOT EXISTS "${WHISPER_ZIP_PATH}")
        file(COPY "${CMAKE_SOURCE_DIR}/build_vs/v1.9.1.zip" DESTINATION "${THIRDPARTY_DIR}")
    endif()

    if(EXISTS "${WHISPER_ZIP_PATH}")
        message(STATUS "Found local whisper.cpp zip in thirdparty: ${WHISPER_ZIP_PATH}. Extracting...")
        file(ARCHIVE_EXTRACT INPUT "${WHISPER_ZIP_PATH}" DESTINATION "${THIRDPARTY_DIR}")
        file(RENAME "${THIRDPARTY_DIR}/whisper.cpp-1.9.1" "${WHISPER_SRC_DIR}")
    else()
        message(STATUS "Downloading whisper.cpp v1.9.1 to thirdparty...")
        file(DOWNLOAD "https://ghproxy.net/https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.1.zip" "${WHISPER_ZIP_PATH}" SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${WHISPER_ZIP_PATH}" DESTINATION "${THIRDPARTY_DIR}")
        file(RENAME "${THIRDPARTY_DIR}/whisper.cpp-1.9.1" "${WHISPER_SRC_DIR}")
    endif()
endif()

# Setup whisper build parameters and add subdirectory
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "Disable whisper tests" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "Disable whisper examples" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "Disable whisper server" FORCE)
option(WHISPER_POC_ENABLE_CUDA "Enable CUDA backend for whisper.cpp" ON)
if(WHISPER_POC_ENABLE_CUDA)
    if(NOT DEFINED CUDAToolkit_ROOT)
        if(EXISTS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1")
            set(CUDAToolkit_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1" CACHE PATH "CUDA Toolkit root path" FORCE)
        elseif(EXISTS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.9")
            set(CUDAToolkit_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.9" CACHE PATH "CUDA Toolkit root path" FORCE)
        elseif(EXISTS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8")
            set(CUDAToolkit_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8" CACHE PATH "CUDA Toolkit root path" FORCE)
        elseif(EXISTS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.6")
            set(CUDAToolkit_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.6" CACHE PATH "CUDA Toolkit root path" FORCE)
        endif()
    endif()
    set(GGML_CUDA ON CACHE BOOL "Enable ggml CUDA backend" FORCE)
    message(STATUS "whisper_poc: CUDA backend enabled")
else()
    set(GGML_CUDA OFF CACHE BOOL "Enable ggml CUDA backend" FORCE)
    message(STATUS "whisper_poc: CUDA backend disabled (CPU only)")
endif()
add_subdirectory("${WHISPER_SRC_DIR}" "${CMAKE_BINARY_DIR}/_deps/whisper-build" EXCLUDE_FROM_ALL)


# 2. Setup ONNX Runtime from local zip inside thirdparty, otherwise download
set(ONNXRUNTIME_ZIP_PATH "${THIRDPARTY_DIR}/onnxruntime.zip")
set(ONNXRUNTIME_DIR "${THIRDPARTY_DIR}/onnxruntime")

if(NOT EXISTS "${ONNXRUNTIME_DIR}")
    # Check if we have the cache zip from the build folder
    if(EXISTS "${CMAKE_SOURCE_DIR}/build_vs/onnxruntime.zip" AND NOT EXISTS "${ONNXRUNTIME_ZIP_PATH}")
        file(COPY "${CMAKE_SOURCE_DIR}/build_vs/onnxruntime.zip" DESTINATION "${THIRDPARTY_DIR}")
    endif()

    if(EXISTS "${ONNXRUNTIME_ZIP_PATH}")
        message(STATUS "Found local ONNX Runtime zip in thirdparty: ${ONNXRUNTIME_ZIP_PATH}. Extracting...")
        file(ARCHIVE_EXTRACT INPUT "${ONNXRUNTIME_ZIP_PATH}" DESTINATION "${THIRDPARTY_DIR}")
        file(RENAME "${THIRDPARTY_DIR}/onnxruntime-win-x64-1.17.3" "${ONNXRUNTIME_DIR}")
    else()
        message(STATUS "Downloading ONNX Runtime 1.17.3 to thirdparty...")
        file(DOWNLOAD "https://ghproxy.net/https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-win-x64-1.17.3.zip" "${ONNXRUNTIME_ZIP_PATH}" SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${ONNXRUNTIME_ZIP_PATH}" DESTINATION "${THIRDPARTY_DIR}")
        file(RENAME "${THIRDPARTY_DIR}/onnxruntime-win-x64-1.17.3" "${ONNXRUNTIME_DIR}")
    endif()
endif()

# Setup ONNX Runtime Include and Library paths
set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_DIR}/include")
set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_DIR}/lib")

if(WIN32)
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_LIB_DIR}/onnxruntime.lib")
    set(ONNXRUNTIME_DLL "${ONNXRUNTIME_LIB_DIR}/onnxruntime.dll")
else()
    set(ONNXRUNTIME_LIB "onnxruntime")
endif()


# 3. Define our executable
add_executable(whisper_poc
    src/main.cpp
)

# Global compiler settings for MSVC
if(MSVC)
    target_compile_options(whisper_poc PRIVATE /utf-8)
    target_compile_definitions(whisper_poc PRIVATE NOMINMAX)
endif()

# Include directories
target_include_directories(whisper_poc PRIVATE
    src
    ${ONNXRUNTIME_INCLUDE_DIR}
    ${WHISPER_SRC_DIR}/include
    ${WHISPER_SRC_DIR}
)

# Link libraries
target_link_libraries(whisper_poc PRIVATE
    whisper
    ${ONNXRUNTIME_LIB}
    ole32
    uuid
    mfplat
    mfreadwrite
    mfuuid
)

# Copy dependent DLLs to the output directory of whisper_poc
if(WIN32)
    add_custom_command(TARGET whisper_poc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_DLL}"
        "$<TARGET_FILE_DIR:whisper_poc>/"
    )
    add_custom_command(TARGET whisper_poc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:whisper>"
        "$<TARGET_FILE:ggml>"
        "$<TARGET_FILE:ggml-cpu>"
        "$<TARGET_FILE:ggml-base>"
        "$<TARGET_FILE_DIR:whisper_poc>/"
    )
    add_custom_command(TARGET whisper_poc POST_BUILD
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
        "if (Test-Path '${CMAKE_SOURCE_DIR}/lib') { Get-ChildItem -LiteralPath '${CMAKE_SOURCE_DIR}/lib' -Filter '*.dll' -File | Copy-Item -Destination '$<TARGET_FILE_DIR:whisper_poc>' -Force }"
        VERBATIM
    )
    add_custom_command(TARGET whisper_poc POST_BUILD
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
        "if (Test-Path '${CMAKE_SOURCE_DIR}/thirdparty/models') { Get-ChildItem -LiteralPath '${CMAKE_SOURCE_DIR}/thirdparty/models' -File | Copy-Item -Destination '$<TARGET_FILE_DIR:whisper_poc>' -Force }"
        VERBATIM
    )
    if(TARGET ggml-cuda)
        add_custom_command(TARGET whisper_poc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ggml-cuda>"
            "$<TARGET_FILE_DIR:whisper_poc>/"
        )
        if(DEFINED CUDAToolkit_ROOT)
            file(GLOB CUDA_RUNTIME_DLLS
                "${CUDAToolkit_ROOT}/bin/cudart64_*.dll"
                "${CUDAToolkit_ROOT}/bin/cublas64_*.dll"
                "${CUDAToolkit_ROOT}/bin/cublasLt64_*.dll"
                "${CUDAToolkit_ROOT}/bin/x64/cudart64_*.dll"
                "${CUDAToolkit_ROOT}/bin/x64/cublas64_*.dll"
                "${CUDAToolkit_ROOT}/bin/x64/cublasLt64_*.dll"
            )
            if(CUDA_RUNTIME_DLLS)
                add_custom_command(TARGET whisper_poc POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CUDA_RUNTIME_DLLS}
                    "$<TARGET_FILE_DIR:whisper_poc>/"
                )
            endif()
        endif()
    endif()
endif()

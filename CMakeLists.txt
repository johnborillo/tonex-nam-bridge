cmake_minimum_required(VERSION 3.24)
project(tonex_nam_bridge CXX)

set(NAMBRIDGE_VERSION "0.1.0")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
endif()
if(MSVC)
  # Static CRT: the ASIO driver DLL is loaded into other people's processes; don't depend on
  # a matching VC++ redistributable being installed.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  add_compile_options(/utf-8 /bigobj)
endif()

set(NAM_DIR ${CMAKE_SOURCE_DIR}/deps/NeuralAmpModelerCore)
set(DSPTOOLS_DIR ${NAM_DIR}/Dependencies/AudioDSPTools/dsp)

if(MSVC)
  set(NB_OPT_NAM /O2 /fp:fast /W0)
  set(NB_OPT /O2 /W3)
  set(NB_QUIET /W0)
else()
  set(NB_OPT_NAM -O3 -ffast-math -Wno-everything)
  set(NB_OPT -O3 -Wall -Wextra -Wno-unused-parameter)
  set(NB_QUIET -Wno-everything)
endif()

# ---- NAM core (static) ------------------------------------------------------------------
file(GLOB NAM_SOURCES ${NAM_DIR}/NAM/*.cpp ${NAM_DIR}/NAM/*/*.cpp)
add_library(namcore STATIC ${NAM_SOURCES})
target_include_directories(namcore SYSTEM PUBLIC
  ${NAM_DIR}
  ${NAM_DIR}/Dependencies/eigen
  ${NAM_DIR}/Dependencies/nlohmann)
target_compile_definitions(namcore PUBLIC NAM_ENABLE_A2_FAST)
target_compile_options(namcore PRIVATE ${NB_OPT_NAM})

# ---- engine (shared by CLI and drivers) --------------------------------------------------
add_library(nambridge_engine STATIC engine/NamEngine.cpp)
target_include_directories(nambridge_engine PUBLIC engine ${DSPTOOLS_DIR})
# NAM registers its architectures (WaveNet, LSTM, ...) from static initializers. Link the
# whole archive so the linker can't drop them (symptom: "No config parser registered for
# architecture: WaveNet"). WHOLE_ARCHIVE = -force_load on Apple, /WHOLEARCHIVE on MSVC.
target_link_libraries(nambridge_engine PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,namcore>")
target_compile_options(nambridge_engine PRIVATE ${NB_OPT})

# ---- CLI (render + selftest everywhere; CoreAudio commands on macOS) ---------------------
add_executable(nam-bridge cli/main.cpp ${DSPTOOLS_DIR}/wav.cpp)
target_link_libraries(nam-bridge PRIVATE nambridge_engine)
set_source_files_properties(${DSPTOOLS_DIR}/wav.cpp PROPERTIES COMPILE_OPTIONS "${NB_QUIET}")
if(APPLE)
  target_link_libraries(nam-bridge PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
endif()

# ---- macOS: AudioServerPlugIn driver + harness ------------------------------------------
if(APPLE)
  add_subdirectory(deps/libASPL EXCLUDE_FROM_ALL)

  set(DRIVER_NAME "NAMBridge")
  set(DRIVER_UID "9E584BAE-0548-4483-AC16-58597E783DF3")
  set(DRIVER_ENTRYPOINT "NAMBridgeEntryPoint")
  configure_file(driver/Info.plist.in ${CMAKE_BINARY_DIR}/driver-Info.plist.in @ONLY)

  add_library(${DRIVER_NAME} MODULE driver/Driver.cpp)
  target_link_libraries(${DRIVER_NAME} PRIVATE nambridge_engine libASPL "-framework CoreFoundation" "-framework CoreAudio")
  target_compile_definitions(${DRIVER_NAME} PRIVATE NAMBRIDGE_VERSION="${NAMBRIDGE_VERSION}")
  target_compile_options(${DRIVER_NAME} PRIVATE -O3 -Wall -Wno-unused-parameter -Wno-deprecated-anon-enum-enum-conversion)
  # Export only the factory: keeps NAM/Eigen/libASPL symbols private to this plug-in so they can
  # never coalesce with another copy loaded in the same audio-server process.
  target_link_options(${DRIVER_NAME} PRIVATE "LINKER:-exported_symbols_list,${CMAKE_SOURCE_DIR}/driver/exports.txt")
  set_target_properties(${DRIVER_NAME} PROPERTIES
    OUTPUT_NAME ${DRIVER_NAME}
    BUNDLE TRUE
    BUNDLE_EXTENSION driver
    PREFIX ""
    SUFFIX ""
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/driver-Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME ${DRIVER_NAME}
    MACOSX_BUNDLE_BUNDLE_VERSION ${NAMBRIDGE_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${NAMBRIDGE_VERSION}
    MACOSX_BUNDLE_COPYRIGHT "tonex-nam-bridge"
    MACOSX_BUNDLE_GUI_IDENTIFIER "local.tonex-nam-bridge.driver")
  # Ad-hoc signature so coreaudiod will load a locally built bundle.
  add_custom_command(TARGET ${DRIVER_NAME} POST_BUILD
    COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:${DRIVER_NAME}>
    VERBATIM)

  add_executable(driver_harness tests/driver_harness.cpp)
  target_link_libraries(driver_harness PRIVATE nambridge_engine "-framework CoreAudio" "-framework CoreFoundation")
  add_dependencies(driver_harness ${DRIVER_NAME})
endif()

# ---- Windows: ASIO driver (see docs/WINDOWS_ASIO.md) ------------------------------------
if(WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/asio/CMakeLists.txt)
  add_subdirectory(asio)
endif()

# NAM Bridge virtual ASIO driver (Windows x64). See docs/WINDOWS_ASIO.md.
# GPLv3 (it uses Steinberg's ASIO SDK under its GPLv3 option): see asio/COPYING.md.

set(ASIOSDK_DIR ${CMAKE_SOURCE_DIR}/deps/asiosdk)
if(NOT EXISTS ${ASIOSDK_DIR}/common/iasiodrv.h)
  message(WARNING "ASIO SDK not found: extract it so that deps/asiosdk/common/iasiodrv.h exists. Skipping the ASIO driver.")
  return()
endif()

add_library(NAMBridgeASIO SHARED
  AsioDriver.cpp
  ComServer.cpp
  ControlPanel.cpp
  Settings.cpp
  exports.def)
target_include_directories(NAMBridgeASIO PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${ASIOSDK_DIR}/common)
target_compile_definitions(NAMBridgeASIO PRIVATE UNICODE _UNICODE NAMBRIDGE_VERSION="${NAMBRIDGE_VERSION}")
target_compile_options(NAMBridgeASIO PRIVATE ${NB_OPT})
target_link_libraries(NAMBridgeASIO PRIVATE
  nambridge_engine ole32 oleaut32 uuid advapi32 user32 comdlg32 comctl32 avrt winmm)
set_target_properties(NAMBridgeASIO PROPERTIES OUTPUT_NAME NAMBridgeASIO)

# Fake ASIO host that proves the driver before ToneX (milestone 3).
add_executable(asio_harness ${CMAKE_SOURCE_DIR}/tests/asio_harness.cpp ${DSPTOOLS_DIR}/wav.cpp)
set_source_files_properties(${DSPTOOLS_DIR}/wav.cpp PROPERTIES COMPILE_OPTIONS "${NB_QUIET}")
target_include_directories(asio_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${ASIOSDK_DIR}/common)
target_compile_definitions(asio_harness PRIVATE UNICODE _UNICODE)
target_compile_options(asio_harness PRIVATE ${NB_OPT})
target_link_libraries(asio_harness PRIVATE nambridge_engine ole32 uuid advapi32)
add_dependencies(asio_harness NAMBridgeASIO)

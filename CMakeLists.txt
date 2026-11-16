cmake_minimum_required(VERSION 3.16)

# Target board (see boards/README.md):
#   175c  Waveshare ESP32-S3-Touch-AMOLED-1.75C (default)
#   175   Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75-B / 1.75-G
#   206   Waveshare ESP32-S3-Touch-AMOLED-2.06
# idf.py -B build-206 -D BOARD=206 build
set(BOARD "175c" CACHE STRING "Target board: 175c, 175 or 206")
set(WATCH_BOARDS 175c 175 206)
if(NOT BOARD IN_LIST WATCH_BOARDS)
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Use one of: ${WATCH_BOARDS}")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One component lock file per board: each board pulls a different board package.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.${BOARD}.lock")
project(electric_pocket_watch)
message(STATUS "Electric Pocket Watch board: ${BOARD}")

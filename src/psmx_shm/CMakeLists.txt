#
# Copyright(c) 2025 ZettaScale Technology and others
#
# This program and the accompanying materials are made available under the
# terms of the Eclipse Public License v. 2.0 which is available at
# http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
# v. 1.0 which is available at
# http://www.eclipse.org/org/documents/edl-v10.php.
#
# SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
#

include(GenerateExportHeader)

message(STATUS "Building Shared Memory PSMX plugin")

set(psmx_shm_sources
  src/psmx_shm_impl.c
  include/psmx_shm_impl.h)

if(BUILD_SHARED_LIBS)
  add_library(psmx_shm SHARED ${psmx_shm_sources})
else()
  add_library(psmx_shm OBJECT ${psmx_shm_sources})
  set_property(GLOBAL APPEND PROPERTY cdds_plugin_list psmx_shm)
  set_property(GLOBAL PROPERTY psmx_shm_symbols shm_create_psmx)
endif()

set_target_properties(psmx_shm PROPERTIES VERSION ${PROJECT_VERSION})
generate_export_header(psmx_shm BASE_NAME DDS_PSMX_SHM EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/psmx_shm_export.h")

target_include_directories(psmx_shm PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/src/ddsrt/include>"
  "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/src/core/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../ddsrt/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/ddsc/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/ddsi/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

# Link pthread for POSIX thread synchronization primitives
find_package(Threads REQUIRED)
target_link_libraries(psmx_shm PRIVATE Threads::Threads)

# Link rt for POSIX shared memory (shm_open, shm_unlink)
target_link_libraries(psmx_shm PRIVATE rt)

if(BUILD_SHARED_LIBS)
  target_link_libraries(psmx_shm PRIVATE ddsc)
endif()

if(MSVC)
  target_compile_definitions(psmx_shm PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS psmx_shm
  EXPORT "${PROJECT_NAME}"
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

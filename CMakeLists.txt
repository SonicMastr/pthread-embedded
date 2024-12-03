cmake_minimum_required(VERSION 3.7)

project(pthread-embedded-vita LANGUAGES C)

set(CMAKE_C_STANDARD 99)

option(BUILD_TESTSUITE "Build testsuite" OFF)
option(MODULE "Build as SUPRX for PS Vita" OFF)
option(STANDALONE_BUILD "Build without SceLibcPosix (Only if building a Module)" ON)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

file(GLOB PLATFORM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform/vita/vita_osal.c)

set(VITA_APP_NAME "PTHREAD TEST")
set(VITA_TITLEID  "PTRD00000")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

file(GLOB SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c)
file(GLOB HELPER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform/helper/*.c)

list(APPEND TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform/vita/main.c)

if (STANDALONE_BUILD)
  set(crt0_SOURCE $ENV{VITASDK}/arm-vita-eabi/lib/crt0_module.o)
else()
  set(crt0_SOURCE $<TARGET_OBJECTS:crt0_module>)
endif()

if (MODULE)
add_executable(pthread
  ${crt0_SOURCE}
  ${SOURCES}
  ${PLATFORM_SOURCES}
)

add_dependencies(pthread SceLibcExt_stub_weak SceLibcPosix)

set_target_properties(pthread PROPERTIES LINKER_LANGUAGE C)

target_include_directories(pthread PRIVATE ${CMAKE_SOURCE_DIR})

target_compile_options(pthread PRIVATE -fno-strict-aliasing -fno-lto)

target_include_directories(pthread PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/platform/vita/)

target_link_libraries(pthread PRIVATE
    -nostdlib
    -nostartfiles
    -nodefaultlibs
    SceLibcPosix_stub_weak
    SceRtc_stub
    SceNetCtl_stub_weak
    SceNet_stub_weak
    SceProcessmgr_stub
    SceSysmodule_stub
    SceKernelThreadMgr_stub
    SceIofilemgr_stub_weak
    SceFios2_stub_weak
    SceLibcExt_stub_weak
    SceLibKernel_stub
    gcc
) 

vita_create_self(pthread.suprx pthread PRIVATE
    CONFIG exports.yml
    UNSAFE
)

vita_create_stubs(pthread_stubs pthread ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml)

else()
add_library(pthread STATIC
  ${SOURCES}
  ${PLATFORM_SOURCES}
)
endif()

if (BUILD_TESTSUITE)

  add_executable(pthread-test
    ${TEST_SOURCES}
  )

  target_link_libraries(pthread-test pthread)

  vita_create_self(pthread-test.self pthread-test UNSAFE)

  vita_create_vpk(pthread-test.vpk ${VITA_TITLEID} pthread-test.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE platform/vita/sce_sys sce_sys
  )
endif()

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/pthread_public.h
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include/
    RENAME pthread.h
)

install(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/sched.h ${CMAKE_CURRENT_SOURCE_DIR}/semaphore.h
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include/
)

install(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/vita/pte_types.h
  DESTINATION ${CMAKE_INSTALL_PREFIX}/include/
)


if (MODULE)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/stubs/
    DESTINATION lib
    FILES_MATCHING PATTERN "*.a"
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pthread.suprx
    DESTINATION "$ENV{VITASDK}/arm-vita-eabi/modules"
  )
else()
  install(TARGETS pthread ARCHIVE)
endif()

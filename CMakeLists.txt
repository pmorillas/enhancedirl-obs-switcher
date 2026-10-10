cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  # Qt 6's OpenGL wrapper still lists the AGL framework, which macOS SDKs from
  # Xcode 26 no longer ship. We don't use it: drop it so the link succeeds.
  if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_eirl_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_eirl_gl_libs)
      list(FILTER _eirl_gl_libs EXCLUDE REGEX "AGL")
      set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_eirl_gl_libs}")
    endif()
  endif()
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/switcher-service.c
    src/agent/agent.cpp
    src/agent/obs-bridge.cpp
    src/rtmp-output/rtmp-stream.c
    src/rtmp-output/flv-mux.c
    src/rtmp-output/rtmp-av1.c
    src/rtmp-output/rtmp-hevc.c
    src/rtmp-output/net-if.c
    src/rtmp-output/rtmp-windows.c
    src/rtmp-output/librtmp/amf.c
    src/rtmp-output/librtmp/cencode.c
    src/rtmp-output/librtmp/log.c
    src/rtmp-output/librtmp/md5.c
    src/rtmp-output/librtmp/parseurl.c
    src/rtmp-output/librtmp/rtmp.c
    src/shared/happy-eyeballs/happy-eyeballs.c
)

# Salida RTMP derivada de obs-outputs (OBS 31.1.1), con RTMPS por mbedTLS (el de las
# dependencias de OBS en Windows/macOS, el del sistema en Linux: el mismo que usa OBS).
# hashswf.c (verificación SWF, de la época de Flash) queda fuera: sin USE_HASHSWF nadie
# la llama y así no hace falta zlib.
find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h REQUIRED)
find_library(MBEDTLS_LIBRARY mbedtls REQUIRED)
find_library(MBEDX509_LIBRARY mbedx509 REQUIRED)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto REQUIRED)
set(MBEDTLS_LIBRARIES ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY})
if(ENABLE_QT)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/ui/switcher-dialog.cpp)
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/ui/switcher-dialog-stub.c)
endif()

target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src src/rtmp-output src/shared/happy-eyeballs ${MBEDTLS_INCLUDE_DIR}
)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CRYPTO USE_MBEDTLS ENABLE_HEVC)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${MBEDTLS_LIBRARIES})
if(OS_MACOS)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "$<LINK_LIBRARY:FRAMEWORK,Foundation.framework>" "$<LINK_LIBRARY:FRAMEWORK,Security.framework>"
  )
endif()

# WebSocket y HTTP del agente (vinculación y conexión con el backend): IXWebSocket
# v11.4.6 (BSD-3), vendorizado en deps/ para compilar sin red (lo exige la imagen
# del Cloud OBS). TLS: mbedTLS en Windows y macOS (el de OBS); OpenSSL en Linux,
# porque esta versión de IXWebSocket necesita mbedTLS >= 3.6 y Ubuntu 24.04 (la base
# del Cloud OBS) trae la 2.28. Sin zlib.
set(USE_TLS ON CACHE BOOL "" FORCE)
if(OS_LINUX)
  set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
  set(USE_MBED_TLS OFF CACHE BOOL "" FORCE)
else()
  set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
  set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
endif()
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/ixwebsocket EXCLUDE_FROM_ALL SYSTEM)
set_target_properties(ixwebsocket PROPERTIES POSITION_INDEPENDENT_CODE ON COMPILE_WARNING_AS_ERROR OFF)
target_compile_options(ixwebsocket PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-w> $<$<CXX_COMPILER_ID:MSVC>:/W0>)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ixwebsocket)
if(OS_LINUX)
  # Las librerías estáticas (IXWebSocket) no exportan nada: no pisan los símbolos
  # que OBS ya tiene cargados.
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--exclude-libs,ALL)
endif()
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-comma -Wno-error -Wno-unused-parameter -Wno-sign-compare -Wno-shorten-64-to-32>
    $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-error>
    $<$<COMPILE_LANG_AND_ID:C,MSVC>:/WX- /wd4100 /wd4996>
)
if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::w32-pthreads iphlpapi winmm ws2_32 crypt32 bcrypt)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

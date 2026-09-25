cmake_minimum_required(VERSION 3.20)
project(RearViewMirror LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Compile settings shared by every target.
add_library(rvm_options INTERFACE)
target_compile_definitions(rvm_options INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(rvm_options INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
target_include_directories(rvm_options INTERFACE src)

# Symbols for release builds too, so a crash report names functions. The code
# generated is the same; the .pdb files sit beside the .exe files.
target_compile_options(rvm_options INTERFACE $<$<CONFIG:Release>:/Zi>)
target_link_options(rvm_options INTERFACE
    $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

# Executables are Authenticode-signed after linking with a self-signed
# certificate that tools/sign.ps1 keeps in the user's certificate store.
option(RVM_SIGN "Sign executables after linking" ON)
if(RVM_SIGN)
    # Without the Windows SDK's signtool there is nothing to sign with; build
    # unsigned rather than fail every link.
    set(_rvm_pf86 "ProgramFiles(x86)")
    file(GLOB _rvm_signtools "$ENV{${_rvm_pf86}}/Windows Kits/10/bin/*/x64/signtool.exe")
    find_program(RVM_SIGNTOOL signtool HINTS "$ENV{WindowsSdkVerBinPath}/x64")
    if(NOT RVM_SIGNTOOL AND NOT _rvm_signtools)
        message(WARNING "signtool.exe not found (install the Windows SDK); executables will not be signed.")
        set(RVM_SIGN OFF)
    endif()
endif()
if(RVM_SIGN)
    # Runs once per build, before any executable links, so parallel signing
    # steps find the certificate rather than racing to create it.
    add_custom_target(rvm_signcert
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tools/sign.ps1"
        COMMENT "Checking the code-signing certificate"
        VERBATIM)
endif()
function(rvm_sign target)
    if(RVM_SIGN)
        add_dependencies(${target} rvm_signcert)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/tools/sign.ps1" -Path "$<TARGET_FILE:${target}>"
            COMMENT "Signing $<TARGET_FILE_NAME:${target}>"
            VERBATIM)
    endif()
endfunction()

# Graphics core: the shared D3D11/D2D device and the composition-hosted window
# base. Used by the app, the client and the tests.
add_library(rvmcore STATIC
    src/gfx.cpp
    src/crash.cpp
    src/overlay.cpp
    src/persist.cpp
)
target_link_libraries(rvmcore PUBLIC rvm_options
    d3d11 dxgi d2d1 dwrite dcomp d3dcompiler
    dwmapi shell32 user32 gdi32 ole32 shcore windowsapp dbghelp
)

# Streaming: the server inside the app, the viewing client, and the tests of
# both. Off builds a mirrors-only app with nothing on the network; the app then
# compiles src/streaming_off.cpp in place of src/streaming.cpp.
option(RVM_STREAMING "Build streaming: the app's server, the client and their tests" ON)

set(RVM_APP_SOURCES
    src/main.cpp
    src/picker.cpp
    src/region.cpp
    src/capture.cpp
    src/renderer.cpp
    src/mirror.cpp
    src/manager.cpp
    src/app.cpp
    src/app.manifest
    src/app.rc
)

if(RVM_STREAMING)
    # Shared by both ends: encryption, UDP transport, packetisation and the
    # H.264 codec.
    add_library(rvmnet STATIC
        src/net/crypto.cpp
        src/net/channel.cpp
        src/net/udp.cpp
        src/net/packetizer.cpp
        src/net/converter.cpp
        src/net/codec.cpp
    )
    target_link_libraries(rvmnet PUBLIC rvmcore bcrypt ws2_32 crypt32 mfplat mf mfuuid)

    add_library(rvmserver STATIC src/stream_server.cpp)
    target_link_libraries(rvmserver PUBLIC rvmnet)

    add_library(rvmclientnet STATIC client/stream_client.cpp)
    target_include_directories(rvmclientnet PUBLIC client)
    target_link_libraries(rvmclientnet PUBLIC rvmnet)

    list(APPEND RVM_APP_SOURCES src/streaming.cpp src/stream_settings.cpp)
else()
    list(APPEND RVM_APP_SOURCES src/streaming_off.cpp)
endif()

add_executable(rearviewmirror WIN32 ${RVM_APP_SOURCES})
if(RVM_STREAMING)
    target_link_libraries(rearviewmirror PRIVATE rvmserver)
else()
    target_link_libraries(rearviewmirror PRIVATE rvmcore)
endif()
set_target_properties(rearviewmirror PROPERTIES OUTPUT_NAME "RearViewMirror")
rvm_sign(rearviewmirror)

if(RVM_STREAMING)
    add_executable(rvmclient WIN32
        client/main.cpp
        client/client_window.cpp
        client/client_config.cpp
        client/connect_dialog.cpp
        client/popout_window.cpp
        src/app.manifest
        client/client.rc
    )
    target_link_libraries(rvmclient PRIVATE rvmclientnet)
    set_target_properties(rvmclient PROPERTIES OUTPUT_NAME "RearViewMirrorClient")
    rvm_sign(rvmclient)

    # Headless tests for the parts that can be exercised without a screen.
    add_executable(rvmnet_test tests/net_test.cpp src/capture.cpp)
    target_link_libraries(rvmnet_test PRIVATE rvmserver rvmclientnet)
endif()

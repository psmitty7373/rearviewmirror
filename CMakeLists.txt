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

# Shared by every executable: the graphics device, the overlay window base,
# logging, crash reports and settings files.
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
option(RVM_REMOTE_CONTROL "Allow focused mouse and keyboard control of full-desktop streams" OFF)
if(RVM_REMOTE_CONTROL AND NOT RVM_STREAMING)
    message(FATAL_ERROR "RVM_REMOTE_CONTROL requires RVM_STREAMING=ON")
endif()
option(RVM_LOGIN_SERVICE "Build the service that streams the sign-in screen while nobody is signed in" OFF)
if(RVM_LOGIN_SERVICE AND NOT RVM_STREAMING)
    message(FATAL_ERROR "RVM_LOGIN_SERVICE requires RVM_STREAMING=ON")
endif()
# Defined as 0 or 1 for every target, so public class layouts agree; test with #if.
target_compile_definitions(rvm_options INTERFACE RVM_LOGIN_SERVICE=$<BOOL:${RVM_LOGIN_SERVICE}>)
target_compile_definitions(rvm_options INTERFACE RVM_REMOTE_CONTROL=$<BOOL:${RVM_REMOTE_CONTROL}>)

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
    if(RVM_REMOTE_CONTROL)
        target_sources(rvmnet PRIVATE src/net/control.cpp)
        add_executable(rvmcontrol_test tests/control_test.cpp)
        target_link_libraries(rvmcontrol_test PRIVATE rvmnet)
    endif()

    add_library(rvmserver STATIC src/stream_server.cpp)
    target_link_libraries(rvmserver PUBLIC rvmnet)

    add_library(rvmclientnet STATIC client/stream_client.cpp)
    target_include_directories(rvmclientnet PUBLIC client)
    target_link_libraries(rvmclientnet PUBLIC rvmnet)

    list(APPEND RVM_APP_SOURCES src/streaming.cpp src/stream_settings.cpp)
    if(RVM_LOGIN_SERVICE)
        list(APPEND RVM_APP_SOURCES src/login_handoff.cpp)
    endif()
else()
    list(APPEND RVM_APP_SOURCES src/streaming_off.cpp)
endif()

add_executable(rearviewmirror WIN32 ${RVM_APP_SOURCES})
if(RVM_STREAMING)
    target_link_libraries(rearviewmirror PRIVATE rvmserver)
    if(RVM_LOGIN_SERVICE)
        target_link_libraries(rearviewmirror PRIVATE wtsapi32)
    endif()
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
    if(RVM_REMOTE_CONTROL)
        target_sources(rvmclient PRIVATE client/control_ui.cpp)
    endif()
    set_target_properties(rvmclient PROPERTIES OUTPUT_NAME "RearViewMirrorClient")
    rvm_sign(rvmclient)

    # Headless tests for the parts that can be exercised without a screen.
    add_executable(rvmnet_test tests/net_test.cpp src/capture.cpp)
    target_link_libraries(rvmnet_test PRIVATE rvmserver rvmclientnet)
endif()

# The sign-in screen service: a console program, for --install and --uninstall.
if(RVM_LOGIN_SERVICE)
    add_executable(rvmservice
        service/main.cpp
        service/install.cpp
        service/supervisor.cpp
        service/login_helper.cpp
        service/duplication.cpp
        service/login_config.cpp
        src/stream_settings.cpp   # Reads the current user's settings at install.
        src/app.manifest
    )
    target_include_directories(rvmservice PRIVATE service)
    target_link_libraries(rvmservice PRIVATE rvmserver wtsapi32 advapi32)
    set_target_properties(rvmservice PROPERTIES OUTPUT_NAME "RearViewMirrorService")
    rvm_sign(rvmservice)
endif()

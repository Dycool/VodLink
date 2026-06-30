cmake_minimum_required(VERSION 3.25)

project(VodLink VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(VODLINK_QT_COMPONENTS
    Widgets
    Sql
    Network
    NetworkAuth
    WebChannel
    WebEngineWidgets
)
find_package(Qt6 6.8 REQUIRED COMPONENTS ${VODLINK_QT_COMPONENTS})
find_package(LibObs REQUIRED)

qt_standard_project_setup()

# Deployment values baked into the binary. Defaults are the project's own
# Google OAuth client ID and Worker URL; CI overrides them from environment
# variables populated by GitHub secrets (when set and non-empty).
set(VODLINK_GOOGLE_CLIENT_ID
    "682238509762-ejh37d898kcl2616k8atmeo095o42plk.apps.googleusercontent.com"
    CACHE STRING "Google OAuth desktop client ID")
set(VODLINK_WORKER_URL
    "https://vodlink.diogoenes0.workers.dev"
    CACHE STRING "VodLink Worker base URL")
# Google's token endpoint requires the client secret in the code exchange even for
# Desktop-app clients using PKCE (Google states this secret is not confidential for
# installed apps). Leave empty only for client types that allow PKCE-only exchange.
set(VODLINK_GOOGLE_CLIENT_SECRET
    ""
    CACHE STRING "Google OAuth desktop client secret")
set(VODLINK_BUILD_COMMIT "dev" CACHE STRING "Source commit embedded in the application")
set(VODLINK_RELEASE_TAG "" CACHE STRING "Release tag, empty for development/Actions builds")
foreach(_var VODLINK_GOOGLE_CLIENT_ID VODLINK_WORKER_URL VODLINK_GOOGLE_CLIENT_SECRET)
    if(DEFINED ENV{${_var}} AND NOT "$ENV{${_var}}" STREQUAL "")
        set(${_var} "$ENV{${_var}}")
    endif()
endforeach()

set(VODLINK_OBS_RUNTIME_DIR "" CACHE PATH
    "Private OBS runtime folder. Windows embeds it in the exe; macOS copies it inside VodLink.app; Linux copies it inside the AppImage payload.")

set(VODLINK_PLATFORM_ICON_SOURCES)
if(WIN32)
    list(APPEND VODLINK_PLATFORM_ICON_SOURCES resources/vodlink.rc)
elseif(APPLE)
    list(APPEND VODLINK_PLATFORM_ICON_SOURCES resources/vodlink.icns)
    set_source_files_properties(resources/vodlink.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

set(VODLINK_EMBEDDED_OBS_RESOURCE_SOURCE)
if(WIN32 AND VODLINK_OBS_RUNTIME_DIR AND EXISTS "${VODLINK_OBS_RUNTIME_DIR}")
    foreach(_vodlink_required_obs_file
            "bin/64bit/obs.dll"
            "bin/64bit/libobs-d3d11.dll"
            "bin/64bit/obs-nvenc-test.exe"
            "bin/64bit/obs-qsv-test.exe"
            "bin/64bit/obs-amf-test.exe"
            "obs-plugins/64bit/obs-outputs.dll"
            "obs-plugins/64bit/rtmp-services.dll"
            "obs-plugins/64bit/obs-ffmpeg.dll"
            "obs-plugins/64bit/win-capture.dll"
            "obs-plugins/64bit/win-wasapi.dll"
            "data/libobs/default.effect"
            "data/libobs/default_rect.effect"
            "data/libobs/opaque.effect"
            "data/libobs/solid.effect"
            "data/libobs/repeat.effect"
            "data/libobs/format_conversion.effect"
            "data/libobs/bicubic_scale.effect"
            "data/libobs/lanczos_scale.effect"
            "data/libobs/area.effect"
            "data/libobs/bilinear_lowres_scale.effect"
            "data/libobs/premultiplied_alpha.effect")
        if(NOT EXISTS "${VODLINK_OBS_RUNTIME_DIR}/${_vodlink_required_obs_file}")
            message(FATAL_ERROR "Private OBS runtime is missing ${_vodlink_required_obs_file}; run scripts/ci/prepare-obs-runtime.ps1 or pass a complete VodLink-owned OBS runtime.")
        endif()
    endforeach()

    file(GLOB_RECURSE VODLINK_OBS_RUNTIME_FILES CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${VODLINK_OBS_RUNTIME_DIR}/*")
    if(VODLINK_OBS_RUNTIME_FILES)
        list(SORT VODLINK_OBS_RUNTIME_FILES)
        set(VODLINK_OBS_RUNTIME_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/obs_runtime_manifest.txt")
        set(VODLINK_OBS_RUNTIME_RC "${CMAKE_CURRENT_BINARY_DIR}/obs_runtime_native.rc")
        set(VODLINK_OBS_RUNTIME_COMPRESSED_DIR "${CMAKE_CURRENT_BINARY_DIR}/obs_runtime_compressed")
        file(MAKE_DIRECTORY "${VODLINK_OBS_RUNTIME_COMPRESSED_DIR}")
        file(WRITE "${VODLINK_OBS_RUNTIME_MANIFEST}" "")
        file(WRITE "${VODLINK_OBS_RUNTIME_RC}" "#include <windows.h>\n")
        find_package(Python3 QUIET COMPONENTS Interpreter)
        set(_vodlink_obs_resource_id 1000)
        foreach(_vodlink_obs_file IN LISTS VODLINK_OBS_RUNTIME_FILES)
            file(RELATIVE_PATH _vodlink_obs_rel "${VODLINK_OBS_RUNTIME_DIR}" "${_vodlink_obs_file}")
            string(REPLACE "\\" "/" _vodlink_obs_rel "${_vodlink_obs_rel}")

            # Raw RCDATA embeds every byte uncompressed, which made the Windows
            # single-exe enormous.  Store each runtime file in Qt qCompress format
            # and inflate it with qUncompress() when extracting to AppData.
            set(_vodlink_obs_resource_kind "raw")
            set(_vodlink_obs_resource_file "${_vodlink_obs_file}")
            if(Python3_Interpreter_FOUND)
                set(_vodlink_obs_compressed "${VODLINK_OBS_RUNTIME_COMPRESSED_DIR}/${_vodlink_obs_resource_id}.bin")
                execute_process(
                    COMMAND "${Python3_EXECUTABLE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/qcompress_file.py"
                            "${_vodlink_obs_file}"
                            "${_vodlink_obs_compressed}"
                    RESULT_VARIABLE _vodlink_obs_compress_result
                )
                if(_vodlink_obs_compress_result EQUAL 0 AND EXISTS "${_vodlink_obs_compressed}")
                    set(_vodlink_obs_resource_kind "z")
                    set(_vodlink_obs_resource_file "${_vodlink_obs_compressed}")
                endif()
            endif()

            file(SHA256 "${_vodlink_obs_file}" _vodlink_obs_sha256)
            file(SIZE "${_vodlink_obs_file}" _vodlink_obs_size)
            string(REPLACE "\\" "/" _vodlink_obs_file_rc "${_vodlink_obs_resource_file}")
            file(APPEND "${VODLINK_OBS_RUNTIME_MANIFEST}" "${_vodlink_obs_resource_id}\t${_vodlink_obs_resource_kind}\t${_vodlink_obs_rel}\t${_vodlink_obs_sha256}\t${_vodlink_obs_size}\n")
            file(APPEND "${VODLINK_OBS_RUNTIME_RC}" "${_vodlink_obs_resource_id} RCDATA \"${_vodlink_obs_file_rc}\"\n")
            math(EXPR _vodlink_obs_resource_id "${_vodlink_obs_resource_id} + 1")
        endforeach()
        string(REPLACE "\\" "/" _vodlink_obs_manifest_rc "${VODLINK_OBS_RUNTIME_MANIFEST}")
        file(APPEND "${VODLINK_OBS_RUNTIME_RC}" "100 RCDATA \"${_vodlink_obs_manifest_rc}\"\n")
        set(VODLINK_EMBEDDED_OBS_RESOURCE_SOURCE "${VODLINK_OBS_RUNTIME_RC}")
    endif()
endif()

qt_add_executable(vodlink WIN32
    src/main.cpp
    src/app/AppController.cpp
    src/app/AppController.h
    src/app/AppPaths.cpp
    src/app/AppPaths.h
    src/app/Config.cpp
    src/app/Config.h
    src/app/DebugLog.cpp
    src/app/DebugLog.h
    src/auth/GoogleAuth.cpp
    src/auth/GoogleAuth.h
    src/cloud/SessionClient.cpp
    src/cloud/SessionClient.h
    src/streaming/RtmpStreamer.cpp
    src/streaming/RtmpStreamer.h
    src/streaming/obs/ObsRuntime.cpp
    src/streaming/obs/ObsRuntime.h
    src/youtube/YouTubeLiveClient.cpp
    src/youtube/YouTubeLiveClient.h
    src/games/GameDefinition.h
    src/games/GameCatalog.cpp
    src/games/GameCatalog.h
    src/games/GameLibrary.cpp
    src/games/GameLibrary.h
    src/games/GameDetector.cpp
    src/games/GameDetector.h
    src/games/ForegroundProbe.cpp
    src/games/ForegroundProbe.h
    src/library/Vod.h
    src/library/VodRepository.cpp
    src/library/VodRepository.h
    src/player/PlayerBridge.cpp
    src/player/PlayerBridge.h
    src/player/SyncPlayer.cpp
    src/player/SyncPlayer.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    resources/resources.qrc
    ${VODLINK_PLATFORM_ICON_SOURCES}
    ${VODLINK_EMBEDDED_OBS_RESOURCE_SOURCE}
)

target_include_directories(vodlink PRIVATE src)
target_compile_definitions(vodlink PRIVATE
    VODLINK_BUILD_COMMIT="${VODLINK_BUILD_COMMIT}"
    VODLINK_RELEASE_TAG="${VODLINK_RELEASE_TAG}"
)

# OBS 32 still exposes obs_add_data_path(), but marks it deprecated. VodLink uses
# it only as a compatibility bridge for the private bundled runtime data path.
# Keep the build logs clean without disabling warnings for the rest of the app.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(src/streaming/RtmpStreamer.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-deprecated-declarations")
endif()

set_target_properties(vodlink PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "VodLink"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.vodlink.desktop"
    MACOSX_BUNDLE_ICON_FILE "vodlink.icns"
)

target_link_libraries(vodlink PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::WebChannel
    Qt6::WebEngineWidgets
    LibObs::libobs
)

# macOS/Linux must be able to resolve libobs from the bundled app payload
# before main() runs. Windows uses delay-load and explicit AddDllDirectory
# after extracting the embedded runtime.
if(APPLE)
    set_target_properties(vodlink PROPERTIES
        MACOSX_RPATH TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks/obs-runtime/bin;@executable_path/../Frameworks/obs-runtime/lib;@executable_path/../Frameworks/obs-runtime/Frameworks;@executable_path/../Frameworks/obs-runtime/PlugIns;@executable_path/../Frameworks/obs-runtime")
elseif(UNIX)
    set_target_properties(vodlink PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "\$ORIGIN/obs-runtime/bin;\$ORIGIN/obs-runtime/lib;\$ORIGIN/obs-runtime/lib64;\$ORIGIN/obs-runtime/usr/lib;\$ORIGIN/obs-runtime/usr/lib/x86_64-linux-gnu;\$ORIGIN/obs-runtime/usr/lib/aarch64-linux-gnu;\$ORIGIN/obs-runtime/usr/local/lib;\$ORIGIN/obs-runtime/usr/local/lib/x86_64-linux-gnu;\$ORIGIN/obs-runtime/usr/local/lib/aarch64-linux-gnu;\$ORIGIN/obs-runtime;\$ORIGIN/obs-runtime/obs-plugins;\$ORIGIN/obs-runtime/obs-plugins/64bit;\$ORIGIN/obs-runtime/plugins;\$ORIGIN/obs-runtime/usr/lib/x86_64-linux-gnu/obs-plugins;\$ORIGIN/obs-runtime/usr/lib/aarch64-linux-gnu/obs-plugins")
endif()

if(WIN32)
    target_link_libraries(vodlink PRIVATE user32 shell32 advapi32 dxgi dxguid delayimp)
    # obs.dll is deliberately delay-loaded. The app extracts its private OBS
    # runtime first, adds that runtime to the DLL search path, then the first
    # libobs call resolves against VodLink's copy only.
    target_link_options(vodlink PRIVATE "/DELAYLOAD:obs.dll")
elseif(APPLE)
    target_link_libraries(vodlink PRIVATE
        "-framework ApplicationServices"
        "-framework AppKit"
        "-framework Foundation"
    )
endif()

target_compile_definitions(vodlink PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    VODLINK_GOOGLE_CLIENT_ID="${VODLINK_GOOGLE_CLIENT_ID}"
    VODLINK_GOOGLE_CLIENT_SECRET="${VODLINK_GOOGLE_CLIENT_SECRET}"
    VODLINK_WORKER_URL="${VODLINK_WORKER_URL}"
)

if(WIN32 AND VODLINK_EMBEDDED_OBS_RESOURCE_SOURCE)
    # Do not use Qt .qrc for the OBS runtime on Windows: embedding a full OBS
    # runtime generates a massive qrc_*.cpp and MSVC can run out of heap. A native
    # .rc/RCDATA resource keeps the runtime inside the exe without compiling a
    # gigantic C++ translation unit.
    target_compile_definitions(vodlink PRIVATE VODLINK_OBS_RUNTIME_EMBEDDED=1)
endif()

# OBS owns the capture, audio, encoder, and RTMP output pipeline. VodLink only
# creates a private scene and private RTMP output; it never reads or writes the
# user's installed OBS Studio profile/config directories.

option(VODLINK_SKIP_QT_DEPLOY "Install only the built executable without Qt runtime deployment" OFF)

install(TARGETS vodlink
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if((APPLE OR UNIX) AND VODLINK_OBS_RUNTIME_DIR AND EXISTS "${VODLINK_OBS_RUNTIME_DIR}")
    if(APPLE)
        install(DIRECTORY "${VODLINK_OBS_RUNTIME_DIR}/"
            DESTINATION "VodLink.app/Contents/Frameworks/obs-runtime")
    else()
        install(DIRECTORY "${VODLINK_OBS_RUNTIME_DIR}/"
            DESTINATION "${CMAKE_INSTALL_BINDIR}/obs-runtime")
    endif()
endif()

if(NOT VODLINK_SKIP_QT_DEPLOY)
    qt_generate_deploy_app_script(
        TARGET vodlink
        OUTPUT_SCRIPT vodlink_deploy_script
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${vodlink_deploy_script})
endif()

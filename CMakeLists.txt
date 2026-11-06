cmake_minimum_required(VERSION 3.24)
project(MinRender VERSION 0.5.11 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ed25519 public key for verifying signed updates. Shared with the sister apps
# (minNotes / QCView) — one private key in the maintainer's Keychain signs every
# release via external/Sparkle/bin/sign_update. Baked into the macOS Info.plist
# (SUPublicEDKey) and the Windows updater (MINRENDER_UPDATE_PUBLIC_KEY) so both
# platforms verify against the same key.
set(MINRENDER_UPDATE_PUBLIC_KEY "+Xlrad3WswxIRTyzBTp8KaTEjyfPO5Ap4llmPdZf3RQ="
    CACHE STRING "Ed25519 public key for Sparkle update verification (macOS)")

# --- FetchContent dependencies (backend-only, no UI toolkit) ---
include(FetchContent)

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# cpp-httplib (header-only HTTP)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# SQLiteCpp
FetchContent_Declare(
    SQLiteCpp
    GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
    GIT_TAG        3.3.2
    GIT_SHALLOW    TRUE
)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_DOXYGEN OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SQLiteCpp)

# min-paths (minplatform canon §22): the family's path kit — cross-OS path translation (with the component-boundary
# check), the share-host probe, file URLs — as a Rust static library built by cargo from the sibling checkout.
include(cmake/MinPaths.cmake)

# --- Qt 6.8 (GUI target only; headless target has no Qt dependency) ---
find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core
    Gui
    Network        # needed for QLocalServer / QLocalSocket in single_instance
    Qml
    Quick
    QuickControls2
    Widgets        # needed for QApplication + QSystemTrayIcon (Phase 1)
)
qt_standard_project_setup(REQUIRES 6.8)

# --- Frame-preview image libraries (GUI target only, OPTIONAL) ---
# Vendored prefix at external/install (decode libs copied from the
# QCView-Player sister project: OpenEXR/Imath + libpng/libjpeg-turbo/
# libtiff). When every package resolves, the minrender target compiles
# src/preview/ (also vendored from QCView) and defines
# MINRENDER_HAVE_PREVIEW; otherwise the preview pane is absent and the
# build proceeds — same optional pattern as Sparkle.
# The prefixes are per-platform and must stay separate: external/install
# holds macOS dylibs whose lib/cmake configs reference gitignored .a
# files, so any other platform picking them up aborts configure.
# external/install-win64 is gitignored;
# repopulate it with `vcpkg export openexr libpng libjpeg-turbo tiff
# --raw` and copy installed/x64-windows/ whole — the vcpkg target
# configs reference the debug/ import libs, so it must come along.
if(APPLE)
    set(MR_VENDOR_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/external/install")
elseif(WIN32)
    set(MR_VENDOR_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/external/install-win64")
endif()
if(MR_VENDOR_PREFIX AND EXISTS "${MR_VENDOR_PREFIX}/lib")
    list(PREPEND CMAKE_PREFIX_PATH "${MR_VENDOR_PREFIX}")
endif()
find_package(OpenEXR 3.1 QUIET)
find_package(Imath QUIET)
find_package(PNG QUIET)
find_package(JPEG QUIET)
find_package(TIFF QUIET)
if(OpenEXR_FOUND AND Imath_FOUND AND PNG_FOUND AND JPEG_FOUND AND TIFF_FOUND)
    set(MR_HAVE_PREVIEW TRUE)
    message(STATUS "minRender: image libs found — frame preview enabled")
else()
    set(MR_HAVE_PREVIEW FALSE)
    message(STATUS "minRender: image libs missing (OpenEXR=${OpenEXR_FOUND} PNG=${PNG_FOUND} JPEG=${JPEG_FOUND} TIFF=${TIFF_FOUND}) — frame preview disabled")
endif()

# --- Core sources (shared between GUI and headless) ---
# NOTE: main_qt.cpp and main_headless.cpp are NOT in this list —
# each is added to exactly one target below. Same for tray: the GUI
# uses src/ui/platform/tray.cpp (Qt), headless keeps the Win32 tray
# in src/core/system_tray.cpp until Phase 8 gives headless Qt too.
set(MINRENDER_CORE_SOURCES
    src/core/platform.cpp
    src/core/node_identity.cpp
    src/core/ipc_server.cpp
    src/core/monitor_log.cpp
    src/core/single_instance.cpp
    src/core/net_utils.cpp
    src/core/sha256.cpp
    src/core/http_server.cpp
    src/core/udp_notify.cpp
    src/monitor/peer_manager.cpp
    src/monitor/monitor_app.cpp
    src/monitor/agent_supervisor.cpp
    src/monitor/template_manager.cpp
    src/monitor/render_coordinator.cpp
    src/monitor/database_manager.cpp
    src/monitor/dispatch_manager.cpp
    src/monitor/farm_init.cpp
    src/monitor/submission_watcher.cpp
)

# --- Platform-specific link libraries ---
if(WIN32)
    set(PLATFORM_LIBS
        shell32 ole32 oleaut32 Rpcrt4 dwmapi Advapi32 Ws2_32 Iphlpapi Mpr
        dxgi bcrypt
    )
elseif(APPLE)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    find_library(IOKIT_LIB IOKit)
    set(PLATFORM_LIBS
        ${COREFOUNDATION_LIB}
        ${IOKIT_LIB}
        pthread
    )
else()
    set(PLATFORM_LIBS pthread)
endif()

# ===========================================================================
# Windows version resources
# ===========================================================================
# Every shipped exe gets an icon + full VERSIONINFO block (company, file
# description, version). Defender's ML heuristics penalize metadata-less
# binaries heavily; Windows ships only through the Microsoft Store, which
# signs the package, so this metadata is the other half of staying clean.
if(WIN32)
    function(mr_add_windows_rc target description original_filename)
        set(MR_RC_DESCRIPTION "${description}")
        set(MR_RC_ORIGINAL_FILENAME "${original_filename}")
        set(MR_RC_INTERNAL_NAME "${target}")
        set(MR_RC_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/minrender.ico")
        configure_file(resources/version.rc.in
            "${CMAKE_CURRENT_BINARY_DIR}/rc/${target}.rc" @ONLY)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/rc/${target}.rc")
    endfunction()
endif()

# ===========================================================================
# MinRender Monitor (Qt 6 Quick GUI)
# ===========================================================================
qt_add_executable(minrender
    ${MINRENDER_CORE_SOURCES}
    src/monitor/main_qt.cpp
    src/ui/app_bridge.cpp
    src/ui/models/chunks_model.cpp
    src/ui/models/jobs_model.cpp
    src/ui/models/log_model.cpp
    src/ui/models/nodes_model.cpp
    src/ui/models/templates_model.cpp
    src/ui/painters/frame_grid.cpp
    src/ui/platform/accent_color.cpp
    src/ui/platform/app_restart.cpp
    src/ui/platform/tray.cpp
    src/ui/updater/app_updater.h   # header-only Q_OBJECT — listed so AUTOMOC mocs it
    # The chromeless window (from UFB / minNotes): flags live in Main.qml,
    # the per-OS halves below.
    src/ui/chrome/WindowChrome.h
    src/ui/chrome/WindowChrome.cpp
    $<$<BOOL:${APPLE}>:src/ui/chrome/WindowChrome_mac.mm>
    $<$<NOT:$<BOOL:${APPLE}>>:src/ui/platform/color_space.cpp>
    $<$<BOOL:${APPLE}>:src/ui/platform/color_space_mac.mm>
)
if(WIN32)
    mr_add_windows_rc(minrender "MinRender Render Farm Monitor" "minRender.exe")
endif()

# QML-only UI — we never use QtDesigner .ui forms, so turn AUTOUIC off.
set_target_properties(minrender PROPERTIES AUTOUIC OFF)

# Shipped binary name is "minRender" (the product brand) on every platform,
# while the CMake target stays "minrender" (lowercase) to avoid clashing with
# the MinRenderUi QML module on case-insensitive filesystems. OUTPUT_NAME only
# renames the emitted binary: minRender.app (macOS, exec Contents/MacOS/
# minRender) and minRender.exe (Windows). The internal helpers
# (minrender-headless, mr-agent, mr-restart) keep their lowercase names.
set_target_properties(minrender PROPERTIES OUTPUT_NAME "minRender")

target_include_directories(minrender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(minrender PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    CPPHTTPLIB_THREAD_POOL_COUNT=4
)

# --- Frame preview (optional; see the image-libs block above) ---
if(MR_HAVE_PREVIEW)
    target_sources(minrender PRIVATE
        src/preview/image_loader.cpp
        src/preview/exr_image_loader.cpp
        src/preview/png_image_loader.cpp
        src/preview/jpeg_image_loader.cpp
        src/preview/tiff_image_loader.cpp
        src/preview/memory_mapped_istream.cpp
        src/preview/thumbnail_image_provider.cpp
    )
    target_compile_definitions(minrender PRIVATE MINRENDER_HAVE_PREVIEW)
    target_link_libraries(minrender PRIVATE
        OpenEXR::OpenEXR
        Imath::Imath
        PNG::PNG
        JPEG::JPEG
        TIFF::TIFF
    )
    # The vendored Windows decode libs are DLLs; sync them next to the
    # exe so dev builds run from build\
    # and package.bat's deploy stage can pick them up from one place.
    if(WIN32 AND EXISTS "${MR_VENDOR_PREFIX}/bin")
        file(GLOB MR_PREVIEW_DLLS "${MR_VENDOR_PREFIX}/bin/*.dll")
        add_custom_command(TARGET minrender POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MR_PREVIEW_DLLS} "$<TARGET_FILE_DIR:minrender>"
            COMMENT "Sync preview decode DLLs next to minRender.exe" VERBATIM)
    endif()
endif()

# Mark Theme.qml as a QML singleton explicitly — Qt 6.11's auto-detection
# of `pragma Singleton` doesn't always produce the right qmldir entry,
# which leaves Theme resolving to undefined inside delegates.
set_source_files_properties(src/ui/qml/Theme.qml src/ui/qml/Format.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# QML module — compiled ahead-of-time into the binary via qmlsc.
qt_add_qml_module(minrender
    URI MinRenderUi
    VERSION 1.0
    QML_FILES
        src/ui/qml/Main.qml
        src/ui/qml/Theme.qml
        src/ui/qml/Format.qml
        src/ui/qml/components/DragMouseArea.qml
        src/ui/qml/components/FlatButton.qml
        src/ui/qml/components/FlatToolTip.qml
        src/ui/qml/components/Icon.qml
        src/ui/qml/components/ListSelection.qml
        src/ui/qml/components/MrDialog.qml
        src/ui/qml/components/MrScrollBar.qml
        src/ui/qml/components/PanelHeader.qml
        src/ui/qml/components/Progress.qml
        src/ui/qml/components/ResizableHeaderLabel.qml
        src/ui/qml/components/SectionHeader.qml
        src/ui/qml/components/StatusBadge.qml
        src/ui/qml/components/ThemedMenu.qml
        src/ui/qml/components/ThemedMenuItem.qml
        src/ui/qml/components/ThemedMenuSeparator.qml
        src/ui/qml/components/ToolStrip.qml
        src/ui/qml/panels/FarmCleanupDialog.qml
        src/ui/qml/panels/JobDetailPanel.qml
        src/ui/qml/panels/JobListPanel.qml
        src/ui/qml/panels/JobPreview.qml
        src/ui/qml/panels/LogPanel.qml
        src/ui/qml/panels/NodePanel.qml
        src/ui/qml/panels/PathMappingsDialog.qml
        src/ui/qml/panels/SettingsPanel.qml
        src/ui/qml/panels/SubmissionForm.qml
    RESOURCES
        src/ui/qml/PhosphorIcons.js
    RESOURCE_PREFIX /qt/qml
)

# Embed icons into the binary at :/icons/… — the deploy stage still
# ships resources/ on disk for fonts, but the app window + tray read icons
# from the embedded copy so a missing resources/ dir can't break either.
qt_add_resources(minrender "app_icons"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/minrender.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/minRenderMacOStray.png"
)

target_link_libraries(minrender PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    nlohmann_json::nlohmann_json
    httplib::httplib
    SQLiteCpp
    minrender::minpaths
    ${PLATFORM_LIBS}
)

# Copy resources/ to the platform-appropriate runtime location.
# - macOS .app bundle: Contents/Resources/. Subdirs in Contents/MacOS/
#   violate the bundle layout convention and break codesign on
#   resource walks (codesign expects only Mach-O under MacOS).
# - Windows/Linux: next to the executable at <bin>/resources/, matching
#   the install layout the Store package creates.
if(APPLE)
    set(MR_RUNTIME_RESOURCES_DEST
        "$<TARGET_BUNDLE_CONTENT_DIR:minrender>/Resources")
else()
    set(MR_RUNTIME_RESOURCES_DEST "$<TARGET_FILE_DIR:minrender>/resources")
endif()
add_custom_command(TARGET minrender POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "${MR_RUNTIME_RESOURCES_DEST}"
    COMMENT "Copying resources to build output"
)
# POST_BUILD only runs when minrender relinks, so a resources-only change
# used to leave the old copy in place: the 0.5.8 macOS release shipped
# pre-0.5.8 plug-ins that way. Listing every resource file as a link input
# makes such a change relink (Makefile and Ninja generators).
file(GLOB_RECURSE MR_RESOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/*")
set_property(TARGET minrender APPEND PROPERTY LINK_DEPENDS ${MR_RESOURCE_FILES})

if(WIN32)
    set_target_properties(minrender PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ===========================================================================
# Auto-update (Sparkle, macOS only) — GUI target only
# ===========================================================================
# Windows has no self-updater: the Microsoft Store is the only Windows
# channel and delivers updates itself (Store policy forbids self-updating,
# and the WindowsApps install dir is read-only). AppUpdater::available()
# is false on Windows so the About menu carries no "Check for Updates…".
#
# Sparkle is vendored under external/ (gitignored) and OPTIONAL: when the
# framework is absent the bridge compiles to the header's inline no-ops and
# the app still builds (dev builds without update support). The headless
# sidecar and mr-agent never link the updater — only this `minrender` GUI
# target does, so a render node can never interrupt a job with an update
# prompt.
if(APPLE)
    # Sparkle 2 — fetched via scripts/fetch_sparkle.sh into external/Sparkle.
    # When present: compile the .mm bridge, define MINRENDER_HAVE_SPARKLE, link
    # -framework Sparkle, and ditto the framework into Contents/Frameworks (ditto
    # preserves the Versions symlink layout copy_directory would flatten + break
    # signing). scripts/macos-release.sh re-signs the nested XPC/Autoupdate/
    # Updater helpers before notarization.
    set(MR_SPARKLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/Sparkle")
    if(EXISTS "${MR_SPARKLE_DIR}/Sparkle.framework")
        target_sources(minrender PRIVATE src/ui/updater/sparkle_updater_macos.mm)
        target_compile_definitions(minrender PRIVATE MINRENDER_HAVE_SPARKLE)
        # -F at compile time so `#import <Sparkle/Sparkle.h>` resolves, and at
        # link time so `-framework Sparkle` finds it.
        target_compile_options(minrender PRIVATE "-F${MR_SPARKLE_DIR}")
        target_link_options(minrender PRIVATE "-F${MR_SPARKLE_DIR}")
        target_link_libraries(minrender PRIVATE "-framework Sparkle")
        add_custom_command(TARGET minrender POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:minrender>/../Frameworks"
            COMMAND ditto
                "${MR_SPARKLE_DIR}/Sparkle.framework"
                "$<TARGET_FILE_DIR:minrender>/../Frameworks/Sparkle.framework"
            COMMENT "Embedding Sparkle.framework into minRender.app/Contents/Frameworks"
            VERBATIM)
        # Sparkle's install name is @rpath/Sparkle.framework/…, so dev builds
        # need an rpath at the bundle's Frameworks dir or dyld aborts at
        # launch (release bundles get this rpath from macdeployqt, which
        # tolerates it already existing).
        set_target_properties(minrender PROPERTIES
            BUILD_RPATH "@executable_path/../Frameworks")
        message(STATUS "minRender: Sparkle vendored — macOS auto-update enabled")
    else()
        message(STATUS "minRender: external/Sparkle absent — macOS auto-update disabled (no-op shim)")
    endif()
endif()

# ===========================================================================
# macOS .app bundle wiring
# ===========================================================================
# Without MACOSX_BUNDLE the build emits a bare Mach-O at build/minRender,
# which inherits the launching shell, has no Dock icon, and confuses Qt's
# native tray integration. With MACOSX_BUNDLE the same target produces
# build/minRender.app — a directory that LaunchServices treats as a
# proper application: double-click works, dock icon renders, tray icon
# survives between sessions. (OUTPUT_NAME above brands the binary minRender.)
#
# Resource layout in the bundle:
#   Contents/MacOS/minRender             — Qt UI executable
#   Contents/MacOS/mr-agent              — Rust render agent (POST_BUILD copy)
#   Contents/Frameworks/Sparkle.framework — auto-updater (when vendored)
#   Contents/Resources/...               — fonts, templates, plugins, icons
#                                           (matches applicationDirPath()/..
#                                           /Resources lookups in C++ code)
#   Contents/Resources/minrender.icns    — bundle icon (CFBundleIconFile)
#   Contents/Info.plist                  — generated from the template below
if(APPLE)
    set_target_properties(minrender PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME           "minRender"
        MACOSX_BUNDLE_GUI_IDENTIFIER        "studio.bialkow.minrender"
        MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING   "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_STRING           "minRender ${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE             "minrender.icns"
        MACOSX_BUNDLE_COPYRIGHT             "Copyright © Chris Bialkowski"
    )

    # Pin the .icns into Contents/Resources/ at build time. CFBundleIconFile
    # above names "minrender.icns", which LaunchServices resolves relative
    # to that folder.
    set_source_files_properties(resources/icons/minrender.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(minrender PRIVATE resources/icons/minrender.icns)

    # Ship license attribution inside the bundle (Contents/Resources). The
    # MIT/BSD notices for Sparkle, Qt, etc. must travel with the binary.
    set_source_files_properties(THIRD_PARTY_NOTICES.txt LICENSE
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(minrender PRIVATE THIRD_PARTY_NOTICES.txt LICENSE)

    # Copy the Rust render agent into the bundle alongside the Qt binary.
    # AgentSupervisor on Unix spawns `applicationDirPath / "mr-agent"`,
    # so this lands it where the existing code already looks. CMake doesn't
    # drive cargo — the agent has to be built first via `cargo build
    # --release` from mr-agent/, then cmake gets re-run to pick it up.
    set(MR_AGENT_RELEASE_BIN
        "${CMAKE_CURRENT_SOURCE_DIR}/mr-agent/target/release/mr-agent")
    if(EXISTS "${MR_AGENT_RELEASE_BIN}")
        add_custom_command(TARGET minrender POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MR_AGENT_RELEASE_BIN}"
                "$<TARGET_FILE_DIR:minrender>/mr-agent"
            COMMENT "Copying mr-agent into .app bundle"
            VERBATIM)
        # A rebuilt agent must relink minrender too, or the copy above is skipped.
        set_property(TARGET minrender APPEND PROPERTY LINK_DEPENDS "${MR_AGENT_RELEASE_BIN}")
    else()
        message(WARNING
            "mr-agent release binary not found at ${MR_AGENT_RELEASE_BIN}. "
            "The .app will build but won't render locally until you run "
            "`cargo build --release` from mr-agent/ and re-run cmake.")
    endif()
endif()

# Same for Windows dev builds: AgentSupervisor spawns `mr-agent.exe` next to
# the monitor exe, but cargo leaves it in mr-agent/target/release/, so running
# from build\ used to log "mr-agent.exe not found" until it was copied by
# hand. (Installers were never affected — package.bat stages the agent
# straight from the cargo output.) As on macOS, CMake doesn't drive cargo:
# build the agent first, then re-run cmake to pick it up.
if(WIN32)
    set(MR_AGENT_RELEASE_BIN
        "${CMAKE_CURRENT_SOURCE_DIR}/mr-agent/target/release/mr-agent.exe")
    if(EXISTS "${MR_AGENT_RELEASE_BIN}")
        add_custom_command(TARGET minrender POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MR_AGENT_RELEASE_BIN}"
                "$<TARGET_FILE_DIR:minrender>/mr-agent.exe"
            COMMENT "Sync mr-agent.exe next to minRender.exe" VERBATIM)
        set_property(TARGET minrender APPEND PROPERTY LINK_DEPENDS "${MR_AGENT_RELEASE_BIN}")
    else()
        message(WARNING
            "mr-agent release binary not found at ${MR_AGENT_RELEASE_BIN}. "
            "Dev builds run from build\\ won't render locally until you run "
            "`cargo build --release` from mr-agent\\ and re-run cmake.")
    endif()
endif()

if(MSVC)
    target_compile_options(minrender PRIVATE /W4)
endif()

# ===========================================================================
# MinRender Headless (sidecar — cross-platform, no GUI dependencies)
# ===========================================================================
add_executable(minrender-headless
    ${MINRENDER_CORE_SOURCES}
    src/core/system_tray.cpp
    src/monitor/main_headless.cpp
)
if(WIN32)
    mr_add_windows_rc(minrender-headless "MinRender Headless Monitor" "minrender-headless.exe")
endif()

# Headless doesn't use any Qt UI facilities — just QLockFile +
# QLocalServer/Socket via single_instance.cpp. Disable Qt autogen to
# skip needless moc/uic/rcc passes.
set_target_properties(minrender-headless PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(minrender-headless PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(minrender-headless PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    CPPHTTPLIB_THREAD_POOL_COUNT=4
    CPPHTTPLIB_NO_OPENSSL
    CPPHTTPLIB_NO_BROTLI
    MINRENDER_HEADLESS
)

target_link_libraries(minrender-headless PRIVATE
    Qt6::Core          # for QLockFile in single_instance
    Qt6::Network       # for QLocalServer / QLocalSocket in single_instance
    nlohmann_json::nlohmann_json
    httplib::httplib
    SQLiteCpp
    minrender::minpaths
    ${PLATFORM_LIBS}
)

add_custom_command(TARGET minrender-headless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:minrender-headless>/resources"
    COMMENT "Copying resources to headless build output"
)
# Same relink-on-resource-change as minrender (see MR_RESOURCE_FILES).
set_property(TARGET minrender-headless APPEND PROPERTY LINK_DEPENDS ${MR_RESOURCE_FILES})

if(MSVC)
    target_compile_options(minrender-headless PRIVATE /W4)
endif()

# ===========================================================================
# Restart Helper (Windows only, unchanged)
# ===========================================================================
if(WIN32)
    add_executable(mr-restart src/restart/main.cpp)
    mr_add_windows_rc(mr-restart "MinRender Update Restart Helper" "mr-restart.exe")
    if(MSVC)
        target_compile_options(mr-restart PRIVATE /W4)
        set_target_properties(mr-restart PROPERTIES WIN32_EXECUTABLE TRUE)
        set_target_properties(mr-restart PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    endif()
endif()

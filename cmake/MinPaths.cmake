# min-paths (minplatform canon §22, row 125 — MinRender is the third consumer): the family's path kit as a Rust static
# library + its cbindgen C header, built from the sibling minplatform checkout by cargo. A path dependency while
# minplatform is private; the same file points at a fetched copy once it is public.
set(MINPLATFORM_DIR "${CMAKE_SOURCE_DIR}/../minplatform" CACHE PATH "The minplatform workspace checkout (crates/min-paths-ffi)")
if(NOT EXISTS "${MINPLATFORM_DIR}/crates/min-paths-ffi/Cargo.toml")
    message(FATAL_ERROR "min-paths: no minplatform checkout at ${MINPLATFORM_DIR} (set MINPLATFORM_DIR)")
endif()
set(_mp_ffi "${MINPLATFORM_DIR}/crates/min-paths-ffi")
if(WIN32)
    set(_mp_lib "${MINPLATFORM_DIR}/target/release/min_paths_ffi.lib")
else()
    set(_mp_lib "${MINPLATFORM_DIR}/target/release/libmin_paths_ffi.a")
endif()
file(GLOB_RECURSE _mp_sources CONFIGURE_DEPENDS
    "${MINPLATFORM_DIR}/crates/min-paths/src/*.rs" "${_mp_ffi}/src/*.rs" "${MINPLATFORM_DIR}/crates/min-paths/Cargo.toml" "${_mp_ffi}/Cargo.toml")
find_program(MINRENDER_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
add_custom_command(OUTPUT "${_mp_lib}"
    COMMAND "${MINRENDER_CARGO}" build --release -p min-paths-ffi
    WORKING_DIRECTORY "${MINPLATFORM_DIR}"
    DEPENDS ${_mp_sources}
    COMMENT "cargo build --release -p min-paths-ffi (minplatform)"
    VERBATIM)
add_custom_target(minpaths_cargo DEPENDS "${_mp_lib}")
add_library(minpaths STATIC IMPORTED GLOBAL)
set_target_properties(minpaths PROPERTIES
    IMPORTED_LOCATION "${_mp_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_mp_ffi}/include")
add_dependencies(minpaths minpaths_cargo)
# What the Rust std + objc2 / windows-sys inside the archive need (`cargo rustc -- --print native-static-libs`).
if(APPLE)
    target_link_libraries(minpaths INTERFACE
        "-framework AppKit" "-framework Foundation" "-framework CoreFoundation" "-framework QuartzCore" "-framework CoreVideo"
        "-framework CoreGraphics" "-framework CoreText" "-framework CoreImage" "-framework CoreData" "-framework CloudKit" iconv objc)
elseif(WIN32)
    target_link_libraries(minpaths INTERFACE kernel32 user32 shell32 gdi32 advapi32 ntdll userenv ws2_32 dbghelp)
    # The `windows` crate's import library lives in the cargo registry, not next to the archive.
    file(GLOB _mp_winlib "$ENV{USERPROFILE}/.cargo/registry/src/*/windows_x86_64_msvc-0.53*/lib/windows.0.53.0.lib")
    if(_mp_winlib)
        list(GET _mp_winlib 0 _mp_winlib1)
        target_link_libraries(minpaths INTERFACE "${_mp_winlib1}")
    else()
        message(WARNING "min-paths: windows.0.53.0.lib not found in the cargo registry; the link may miss symbols")
    endif()
endif()
add_library(minrender::minpaths ALIAS minpaths)

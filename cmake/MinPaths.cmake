# min-paths (minplatform canon §22, row 125 — MinRender is the third consumer): the family's path kit as a Rust static library + its cbindgen
# C header, built by cargo from the minplatform workspace at a PINNED revision. By default CMake fetches that revision
# into the build dir (reproducible; minplatform is public). MINPLATFORM_DIR overrides the fetch with a local checkout
# (the sibling clone while the crate itself is being worked on — the pin then does not apply).
set(MINPLATFORM_GIT "https://github.com/cbkow/minplatform.git" CACHE STRING "Where minplatform is fetched from")
set(MINPLATFORM_REV "1d48ee44d2d473c55d5e36ab430d76716a49f248" CACHE STRING "The minplatform revision this app builds against")
set(MINPLATFORM_DIR "" CACHE PATH "A local minplatform checkout to build against instead of the pinned fetch")
if(MINPLATFORM_DIR)
    if(NOT EXISTS "${MINPLATFORM_DIR}/crates/min-paths-ffi/Cargo.toml")
        message(FATAL_ERROR "min-paths: no minplatform checkout at ${MINPLATFORM_DIR} (crates/min-paths-ffi/Cargo.toml missing)")
    endif()
    set(_mp_root "${MINPLATFORM_DIR}")
    message(STATUS "min-paths: local checkout ${_mp_root}")
else()
    include(FetchContent)
    FetchContent_Declare(minplatform
        GIT_REPOSITORY "${MINPLATFORM_GIT}"
        GIT_TAG "${MINPLATFORM_REV}"
        GIT_SHALLOW OFF)   # a commit hash cannot be fetched shallowly
    FetchContent_MakeAvailable(minplatform)   # populate only: minplatform has no root CMakeLists.txt
    set(_mp_root "${minplatform_SOURCE_DIR}")
    message(STATUS "min-paths: minplatform @ ${MINPLATFORM_REV} fetched into ${_mp_root}")
endif()
set(_mp_ffi "${_mp_root}/crates/min-paths-ffi")
if(WIN32)
    set(_mp_lib "${_mp_root}/target/release/min_paths_ffi.lib")
else()
    set(_mp_lib "${_mp_root}/target/release/libmin_paths_ffi.a")
endif()
file(GLOB_RECURSE _mp_sources CONFIGURE_DEPENDS
    "${_mp_root}/crates/min-paths/src/*.rs" "${_mp_ffi}/src/*.rs" "${_mp_root}/crates/min-paths/Cargo.toml" "${_mp_ffi}/Cargo.toml")
find_program(MINRENDER_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
add_custom_command(OUTPUT "${_mp_lib}"
    COMMAND "${MINRENDER_CARGO}" build --release -p min-paths-ffi
    WORKING_DIRECTORY "${_mp_root}"
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

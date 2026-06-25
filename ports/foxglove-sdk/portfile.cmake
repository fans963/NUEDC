vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO foxglove/foxglove-sdk
    REF "sdk/v${VERSION}"
    SHA512 f9e461bd96db024314ed4c8620812adc673b817bf3553fb5137604971c370057eab48376f846750626bdccfc95b115db1b46618dcad068482c88d1bc65533356
    HEAD_REF main
)

# ── Rust target & optimization ───────────────────────────────────────
set(_rust_target "")
set(_cargo_feats "--no-default-features" "--features=ring")
if(NOT "${VCPKG_TARGET_ARCHITECTURE}" STREQUAL "${VCPKG_HOST_ARCHITECTURE}")
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(_rust_target "aarch64-unknown-linux-gnu")
        set(_prebuilt_dir "${SOURCE_PATH}/cargo-prebuild")
    endif()
endif()

# ── Cargo config (shared by both paths) ─────────────────────────────
set(_cargo_cfg "$ENV{HOME}/.cargo/config.toml")
set(_cargo_saved "")
if(EXISTS "${_cargo_cfg}")
    file(READ "${_cargo_cfg}" _cargo_saved)
endif()

# Profile: max optimization (does NOT conflict with Corrosion)
file(WRITE "${_cargo_cfg}"
    "[profile.release]\n"
    "lto = \"fat\"\n"
    "codegen-units = 1\n"
)

function(foxglove_restore_cargo_config)
    if(_cargo_saved)
        file(WRITE "${_cargo_cfg}" "${_cargo_saved}")
    else()
        file(REMOVE "${_cargo_cfg}")
    endif()
endfunction()

# ── Step 1: Build Rust C library ─────────────────────────────────────

if(_rust_target)
    vcpkg_execute_required_process(
        COMMAND rustup target add ${_rust_target}
        WORKING_DIRECTORY "${SOURCE_PATH}"
        LOGNAME rustup-target
    )

    # Append target linker to cargo config
    file(APPEND "${_cargo_cfg}"
        "\n[target.${_rust_target}]\nlinker = \"aarch64-linux-gnu-gcc\"\n"
    )

    # Unset global CC/CXX → Cargo uses host compiler for build scripts
    set(_save_cc  "$ENV{CC}")
    set(_save_cxx "$ENV{CXX}")
    unset(ENV{CC})
    unset(ENV{CXX})

    # RUSTFLAGS for cross-compile (safe: no Corrosion conflict)
    set(_cross_rustflags "-Clto=fat -Ccodegen-units=1 -Ctarget-cpu=cortex-a76")
    set(ENV{RUSTFLAGS} "${_cross_rustflags}")

    message(STATUS "foxglove-sdk: cargo build --release --target=${_rust_target}")
    message(STATUS "foxglove-sdk: RUSTFLAGS=${_cross_rustflags}")
    vcpkg_execute_required_process(
        COMMAND cargo build --release --target ${_rust_target}
                             --manifest-path c/Cargo.toml
                             ${_cargo_feats}
        WORKING_DIRECTORY "${SOURCE_PATH}"
        LOGNAME cargo-build
    )
    unset(ENV{RUSTFLAGS})

    # Verify target-cpu via ARM ELF attributes
    execute_process(
        COMMAND readelf -A "${_cargo_out}/libfoxglove.so"
        OUTPUT_VARIABLE _elf_arch ERROR_QUIET
    )
    if(_elf_arch MATCHES "v8")
        message(STATUS "foxglove-sdk: ARMv8 arch detected ✓")
    endif()

    set(ENV{CC}  "${_save_cc}")
    set(ENV{CXX} "${_save_cxx}")
    foxglove_restore_cargo_config()

    # Copy .a and .so into prebuilt dir
    file(MAKE_DIRECTORY "${_prebuilt_dir}")
    set(_cargo_out "${SOURCE_PATH}/target/${_rust_target}/release")
    file(COPY "${_cargo_out}/libfoxglove.a"  DESTINATION "${_prebuilt_dir}")
    file(COPY "${_cargo_out}/libfoxglove.so" DESTINATION "${_prebuilt_dir}")
    message(STATUS "foxglove-sdk: cargo done → ${_prebuilt_dir}")
endif()

# ── Step 2: Build C++ wrapper via official CMake ─────────────────────

# Native: append target-cpu to cargo config (no RUSTFLAGS, avoids
# conflict with Corrosion's -Cembed-bitcode=no)
if(NOT _rust_target)
    file(APPEND "${_cargo_cfg}"
        "\n[build]\nrustflags = [\"-C\", \"target-cpu=native\"]\n"
    )
    message(STATUS "foxglove-sdk: native (Corrosion), lto=fat codegen-units=1 target-cpu=native")
endif()

set(_cmake_opts
    -DFOXGLOVE_BUILD_EXAMPLES=OFF
    -DFOXGLOVE_BUILD_INTEGRATION_TESTS=OFF
    -DFOXGLOVE_REMOTE_ACCESS=OFF
    -DUSE_PACKAGE_MANAGER_DEPENDENCIES=ON
    -DSTRICT=OFF
    -DCMAKE_WARN_DEPRECATED=OFF
    -DFETCHCONTENT_FULLY_DISCONNECTED=OFF
    -DBUILD_TESTING=OFF
)
if(_rust_target)
    list(APPEND _cmake_opts "-DFOXGLOVE_PREBUILT_LIB_DIR=${_prebuilt_dir}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/cpp"
    OPTIONS ${_cmake_opts}
)

# Restore cargo config after CMake configure (Corrosion reads it then)
if(NOT _rust_target)
    foxglove_restore_cargo_config()
endif()

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME foxglove-sdk
    CONFIG_PATH lib/cmake/foxglove-sdk
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

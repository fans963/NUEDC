vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO foxglove/foxglove-sdk
    REF "sdk/v${VERSION}"
    SHA512 f9e461bd96db024314ed4c8620812adc673b817bf3553fb5137604971c370057eab48376f846750626bdccfc95b115db1b46618dcad068482c88d1bc65533356
    HEAD_REF main
)

# ── Determine Rust target triple ─────────────────────────────────────
set(_rust_target "")
set(_cargo_feats "--no-default-features" "--features=ring")  # ring crypto backend
if(NOT "${VCPKG_TARGET_ARCHITECTURE}" STREQUAL "${VCPKG_HOST_ARCHITECTURE}")
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(_rust_target "aarch64-unknown-linux-gnu")
        set(_prebuilt_dir "${SOURCE_PATH}/cargo-prebuild")
    endif()
endif()

# ── Step 1: Build Rust C library ─────────────────────────────────────

if(_rust_target)
    vcpkg_execute_required_process(
        COMMAND rustup target add ${_rust_target}
        WORKING_DIRECTORY "${SOURCE_PATH}"
        LOGNAME rustup-target
    )

    # Cargo config: per-target linker (not global CC)
    set(_cargo_cfg "$ENV{HOME}/.cargo/config.toml")
    # Save existing config content
    if(EXISTS "${_cargo_cfg}")
        file(READ "${_cargo_cfg}" _cargo_saved)
    endif()
    file(WRITE "${_cargo_cfg}"
        "[target.${_rust_target}]\nlinker = \"aarch64-linux-gnu-gcc\"\n"
    )

    # Unset global CC/CXX so Cargo uses HOST compiler for build scripts
    set(_save_cc  "$ENV{CC}")
    set(_save_cxx "$ENV{CXX}")
    unset(ENV{CC})
    unset(ENV{CXX})

    message(STATUS "foxglove-sdk: cargo build --release --target=${_rust_target}")
    vcpkg_execute_required_process(
        COMMAND cargo build --release --target ${_rust_target}
                             --manifest-path c/Cargo.toml
                             ${_cargo_feats}
        WORKING_DIRECTORY "${SOURCE_PATH}"
        LOGNAME cargo-build
    )

    # Restore environment
    set(ENV{CC}  "${_save_cc}")
    set(ENV{CXX} "${_save_cxx}")
    # Restore cargo config
    if(DEFINED _cargo_saved)
        file(WRITE "${_cargo_cfg}" "${_cargo_saved}")
    else()
        file(REMOVE "${_cargo_cfg}")
    endif()

    # Collect .a and .so into prebuilt dir (explicit filenames, not GLOB)
    file(MAKE_DIRECTORY "${_prebuilt_dir}")
    set(_cargo_out "${SOURCE_PATH}/target/${_rust_target}/release")
    file(COPY "${_cargo_out}/libfoxglove.a"  DESTINATION "${_prebuilt_dir}")
    file(COPY "${_cargo_out}/libfoxglove.so" DESTINATION "${_prebuilt_dir}")
    message(STATUS "foxglove-sdk: cargo done → ${_prebuilt_dir}")
endif()

# ── Step 2: Build C++ wrapper via official CMake ─────────────────────

set(_cmake_opts
    -DFOXGLOVE_BUILD_EXAMPLES=OFF
    -DFOXGLOVE_BUILD_INTEGRATION_TESTS=OFF
    -DFOXGLOVE_REMOTE_ACCESS=OFF
    -DUSE_PACKAGE_MANAGER_DEPENDENCIES=ON
    -DSTRICT=OFF
    -DCMAKE_WARN_DEPRECATED=OFF
    -DFETCHCONTENT_FULLY_DISCONNECTED=OFF
    -DBUILD_TESTING=OFF
    # GCC 16 + libwebsockets v4.3.3: -Werror in CMAKE_C_FLAGS_DEBUG overrides
    # our -Wno-error. Override C debug+release flags to remove -Werror.
    "-DCMAKE_C_FLAGS=${VCPKG_C_FLAGS} -Wno-error=discarded-qualifiers"
    "-DCMAKE_C_FLAGS_DEBUG=-g -Wno-error=discarded-qualifiers"
    "-DCMAKE_C_FLAGS_RELEASE=${VCPKG_C_FLAGS} -Wno-error=discarded-qualifiers"
)

if(_rust_target)
    list(APPEND _cmake_opts "-DFOXGLOVE_PREBUILT_LIB_DIR=${_prebuilt_dir}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/cpp"
    OPTIONS ${_cmake_opts}
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME foxglove-sdk
    CONFIG_PATH lib/cmake/foxglove-sdk
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

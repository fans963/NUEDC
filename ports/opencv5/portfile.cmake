set(VCPKG_POLICY_MISMATCHED_NUMBER_OF_BINARIES enabled)
set(VCPKG_POLICY_SKIP_ABSOLUTE_PATHS_CHECK enabled)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO opencv/opencv
    REF "${VERSION}"
    SHA512 37143de2ba76a9af351d6901358635b42b671b5fbe3ccc75d4d27133b0a6c6a0400564e6eaf228fc1705e25a2af09c8e2638039ca9af0ce9428da253dd31d3e0
    HEAD_REF master
)

# Remove vendored 3rdparty libs except those needed for image I/O and HAL
file(GLOB third_party "${SOURCE_PATH}/3rdparty/*")
foreach(_dir IN LISTS third_party)
    get_filename_component(_name "${_dir}" NAME)
    if(NOT _name MATCHES "^(zlib|libtiff|libwebp|libjpeg-turbo|openjpeg|ippiw|ippicv|carotene|kleidicv|flatbuffers|protobuf|quirc|cpufeatures|dlpack)$")
        file(REMOVE_RECURSE "${_dir}")
    endif()
endforeach()

vcpkg_find_acquire_program(PKGCONFIG)
set(ENV{PKG_CONFIG} "${PKGCONFIG}")

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(TARGET_IS_AARCH64 1)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
    set(TARGET_IS_ARM 1)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(TARGET_IS_X86_64 1)
else()
    set(TARGET_IS_X86 1)
endif()

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" BUILD_WITH_STATIC_CRT)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
    "calib3d"   BUILD_opencv_calib3d
    "contrib"   WITH_CONTRIB
    "dnn"       BUILD_opencv_dnn
    "eigen"     WITH_EIGEN
    "ffmpeg"    WITH_FFMPEG
    "highgui"   BUILD_opencv_highgui
    "imgcodecs" BUILD_opencv_imgcodecs
    "imgproc"   BUILD_opencv_imgproc
    "intrinsics" CV_ENABLE_INTRINSICS
    "jpeg"      WITH_JPEG
    "opencl"    WITH_OPENCL
    "png"       WITH_PNG
    "thread"    OPENCV_ENABLE_THREAD_SUPPORT
    "videoio"   BUILD_opencv_videoio
    "world"     BUILD_opencv_world
)

if("contrib" IN_LIST FEATURES)
    vcpkg_from_github(
        OUT_SOURCE_PATH CONTRIB_SOURCE_PATH
        REPO opencv/opencv_contrib
        REF "${VERSION}"
        SHA512 09841f4637f6a51a5e12a5516710e61a3cedd2622455366004cd9e35484a6d03cc9c12de52edae4072dc37af104d7fd9099201effee82c52dd40b8a58c51fdc5
        HEAD_REF master
    )
    set(BUILD_WITH_CONTRIB_FLAG "-DOPENCV_EXTRA_MODULES_PATH=${CONTRIB_SOURCE_PATH}/modules")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ###### Verify that required components and only those are enabled
        -DENABLE_CONFIG_VERIFICATION=OFF
        ###### opencv cpu recognition is broken, always using host and not target: here we bypass that
        -DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=TRUE
        -DAARCH64=${TARGET_IS_AARCH64}
        -DARM=${TARGET_IS_ARM}
        -DX86_64=${TARGET_IS_X86_64}
        -DX86=${TARGET_IS_X86}
        ###### ocv installation dir options
        -DINSTALL_TO_MANGLED_PATHS=OFF
        -DOpenCV_INSTALL_BINARIES_PREFIX=
        -DOPENCV_BIN_INSTALL_PATH=bin
        -DOPENCV_CONFIG_INSTALL_PATH=share/opencv5
        -DOPENCV_INCLUDE_INSTALL_PATH=include/opencv5
        -DOPENCV_LIB_INSTALL_PATH=lib
        -DOPENCV_3P_LIB_INSTALL_PATH=lib/manual-link/opencv5_thirdparty
        ###### ocv_options
        -DCV_TRACE=OFF
        -DCMAKE_DEBUG_POSTFIX=d
        -DOPENCV_DEBUG_POSTFIX=d
        -DOPENCV_DLLVERSION=5
        -DOPENCV_FFMPEG_USE_FIND_PACKAGE=FFMPEG
        -DOPENCV_FFMPEG_SKIP_BUILD_CHECK=TRUE
        -DOPENCV_FORCE_EIGEN_FIND_PACKAGE_CONFIG=ON
        -DOPENCV_GENERATE_PKGCONFIG=ON
        -DOPENCV_GENERATE_SETUPVARS=OFF
        -DOPENCV_PYTHON2_SKIP_DETECTION=ON
        # Do not build docs/examples/tests
        -DBUILD_DOCS=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_PERF_TESTS=OFF
        -DBUILD_TESTS=OFF
        ###### Disable build 3rd party libs (keep image I/O libs as vendored)
        -DBUILD_IPP_IW=OFF
        -DBUILD_ITT=OFF
        -DBUILD_JASPER=OFF
        -DBUILD_OPENEXR=OFF
        -DBUILD_PROTOBUF=OFF
        -DBUILD_TBB=OFF
        ###### OpenCV Build components
        -DBUILD_opencv_apps=OFF
        -DBUILD_opencv_java=OFF
        -DBUILD_opencv_js=OFF
        -DBUILD_JAVA=OFF
        -DBUILD_ANDROID_PROJECT=OFF
        -DBUILD_ANDROID_EXAMPLES=OFF
        -DBUILD_PACKAGE=OFF
        ###### Disable unused modules (keep flann/geometry: imgproc dependencies)
        -DBUILD_opencv_calib3d=OFF
        -DBUILD_opencv_dnn=OFF
        -DBUILD_opencv_features=OFF
        -DBUILD_opencv_highgui=OFF
        -DBUILD_opencv_objdetect=OFF
        -DBUILD_opencv_photo=OFF
        -DBUILD_opencv_stitching=OFF
        -DBUILD_opencv_video=OFF
        -DBUILD_opencv_stereo=OFF
        -DBUILD_opencv_ptcloud=OFF
        -DBUILD_WITH_DEBUG_INFO=ON
        -DBUILD_WITH_STATIC_CRT=${BUILD_WITH_STATIC_CRT}
        -DCURRENT_INSTALLED_DIR=${CURRENT_INSTALLED_DIR}
        ###### PYLINT/FLAKE8
        -DENABLE_PYLINT=OFF
        -DENABLE_FLAKE8=OFF
        # CMAKE/VCPKG
        -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_JNI=ON
        -DVCPKG_LOCK_FIND_PACKAGE_Iconv=OFF
        ###### OPENCV vars
        "${BUILD_WITH_CONTRIB_FLAG}"
        -DOPENCV_OTHER_INSTALL_PATH=share/opencv5
        ###### ARM performance (Cortex-A76, ARMv8.2-A)
        -DWITH_CAROTENE=${TARGET_IS_AARCH64}
        -DWITH_KLEIDICV=${TARGET_IS_AARCH64}
        -DWITH_CPUFEATURES=${TARGET_IS_AARCH64}
        -DCV_DISABLE_OPTIMIZATION=OFF
        ###### customized properties
        ${FEATURE_OPTIONS}
        -DWITH_AVIF=OFF
        -DWITH_GSTREAMER=OFF
        -DWITH_GTK=OFF
        -DWITH_ITT=OFF
        -DWITH_JASPER=OFF
        -DWITH_LAPACK=OFF
        -DWITH_MATLAB=OFF
        -DWITH_NVCUVID=OFF
        -DWITH_NVCUVENC=OFF
        -DWITH_OBSENSOR=OFF
        -DWITH_OPENCL_D3D11_NV=OFF
        -DWITH_OPENCLAMDBLAS=OFF
        -DWITH_OPENCLAMDFFT=OFF
        -DWITH_OPENJPEG=OFF
        -DWITH_PROTOBUF=OFF
        -DWITH_SPNG=OFF
        -DWITH_UNIFONT=OFF
        -DWITH_VA=OFF
        -DWITH_VA_INTEL=OFF
        -DWITH_ZLIB_NG=OFF
        ###### Additional build flags
        ${ADDITIONAL_BUILD_FLAGS}
    MAYBE_UNUSED_VARIABLES
        OPENCV_FORCE_EIGEN_FIND_PACKAGE_CONFIG
        OPENCV_PYTHON2_SKIP_DETECTION
        VCPKG_LOCK_FIND_PACKAGE_Iconv
)

# Skip debug build — KleidiCV section attributes conflict with GCC debug flags
set(VCPKG_BUILD_TYPE release)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
vcpkg_copy_pdbs()

# Make find_package(OpenCV) work
file(WRITE "${CURRENT_PACKAGES_DIR}/share/opencv/OpenCVConfig.cmake"
"include(CMakeFindDependencyMacro)
find_dependency(Threads)
include(\"\${CMAKE_CURRENT_LIST_DIR}/../opencv5/OpenCVConfig.cmake\")
")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

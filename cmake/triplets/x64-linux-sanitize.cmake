set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(GEOWORLD_SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
set(VCPKG_C_FLAGS "${GEOWORLD_SANITIZER_FLAGS}")
set(VCPKG_CXX_FLAGS "${GEOWORLD_SANITIZER_FLAGS}")
set(VCPKG_LINKER_FLAGS "-fsanitize=address,undefined")

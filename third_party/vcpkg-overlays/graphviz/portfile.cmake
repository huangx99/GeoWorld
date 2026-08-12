vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.com
    OUT_SOURCE_PATH SOURCE_PATH
    REPO graphviz/graphviz
    REF "${VERSION}"
    SHA512 5607ac820258dd05d5de37a935f5a3d8e1d6e03f68533bce2dd990a9fafc1cd854ade2b92e3302423ac6612d20f301ddeafc83769396c348bdae934ee83c9df8
    PATCHES
        "${VCPKG_ROOT_DIR}/ports/graphviz/build.diff"
        "${VCPKG_ROOT_DIR}/ports/graphviz/dependencies.diff"
        "${VCPKG_ROOT_DIR}/ports/graphviz/install.diff"
        "${VCPKG_ROOT_DIR}/ports/graphviz/no-absolute-paths.patch"
        "${VCPKG_ROOT_DIR}/ports/graphviz/skip-configure-plugins.diff"
        "${VCPKG_ROOT_DIR}/ports/graphviz/version.diff"
        install-layout-libraries.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS OPTIONS FEATURES tools GRAPHVIZ_CLI)
foreach(lang IN ITEMS D GO GUILE JAVA JAVASCRIPT LUA PERL PHP PYTHON R RUBY SHARP TCL)
    list(APPEND OPTIONS -DENABLE_${lang}=OFF)
endforeach()
vcpkg_find_acquire_program(BISON)
vcpkg_find_acquire_program(FLEX)
vcpkg_find_acquire_program(PKGCONFIG)
vcpkg_find_acquire_program(PYTHON3)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_PROJECT_INCLUDE=${VCPKG_ROOT_DIR}/ports/graphviz/cmake-project-include.cmake"
        "-DVERSION=${VERSION}"
        "-DBISON_EXECUTABLE=${BISON}"
        "-DFLEX_EXECUTABLE=${FLEX}"
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        "-DPython3_EXECUTABLE=${PYTHON3}"
        -Dinstall_win_dependency_dlls=OFF -Duse_win_pre_inst_libs=OFF
        -DENABLE_LTDL=ON -DENABLE_SWIG=OFF -DWITH_EXPAT=ON -DWITH_GDK=OFF
        -DWITH_GHOSTSCRIPT=OFF -DWITH_GTK=OFF -DWITH_GVEDIT=OFF -DWITH_POPPLER=OFF
        -DWITH_RSVG=ON -DWITH_SMYRNA=OFF -DWITH_WEBP=ON -DWITH_X=OFF -DWITH_ZLIB=ON
        -DVCPKG_LOCK_FIND_PACKAGE_AA=OFF -DVCPKG_LOCK_FIND_PACKAGE_ANN=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_CAIRO=ON -DVCPKG_LOCK_FIND_PACKAGE_DevIL=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_EXPAT=ON -DVCPKG_LOCK_FIND_PACKAGE_Freetype=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_GD=ON -DVCPKG_LOCK_FIND_PACKAGE_GTS=ON
        -DVCPKG_LOCK_FIND_PACKAGE_PANGOCAIRO=ON ${OPTIONS}
    OPTIONS_DEBUG -DGRAPHVIZ_CLI=OFF
    MAYBE_UNUSED_VARIABLES install_win_dependency_dlls
)
vcpkg_cmake_install(ADD_BIN_TO_PATH)
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/graphviz)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share" "${CURRENT_PACKAGES_DIR}/share/man")
if("tools" IN_LIST FEATURES)
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
    foreach(script_or_link IN ITEMS "dot2gxl${VCPKG_TARGET_EXECUTABLE_SUFFIX}" gvmap.sh dot_sandbox)
        if(EXISTS "${CURRENT_PACKAGES_DIR}/bin/${script_or_link}")
            file(RENAME "${CURRENT_PACKAGES_DIR}/bin/${script_or_link}" "${CURRENT_PACKAGES_DIR}/tools/${PORT}/${script_or_link}")
        endif()
    endforeach()
    vcpkg_copy_tools(TOOL_NAMES acyclic bcomps ccomps circo cluster dijkstra dot edgepaint fdp gc gml2gv graphml2gv gv2gml gv2gxl gvcolor gvgen gvmap gvpack gvpr gxl2dot gxl2gv mm2gv neato nop osage patchwork prune sccmap sfdp tred twopi unflatten AUTO_CLEAN)
endif()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

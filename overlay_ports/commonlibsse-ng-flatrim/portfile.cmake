vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Zzyxz/CommonLibSSE-NG
    REF d4ee41104c391ac4e19093c97c3a76138bf6b55a
    SHA512 3628aa145ed358849c10058d3bfd50a829cdc2cb85c2b789400057a5643c85a0488c4b5a78e789ed284d27334a4355bbca7a8e4b78fa875ae2540feaecaeca43
    HEAD_REF main
    PATCHES
      patches/01-objectrefr-make_moverefr_public.patch
      patches/02-variable-make_members_public.patch
      patches/03-stackframe-uncomment_top_args.patch
      patches/04-extradatalist-make_members_public.patch
)

set(SOURCE_PATH "${SOURCE_PATH}/extern/CommonLibSSE")

vcpkg_configure_cmake(
        SOURCE_PATH "${SOURCE_PATH}"
        PREFER_NINJA
        OPTIONS -DENABLE_SKYRIM_VR=off -DBUILD_TESTS=off -DSKSE_SUPPORT_XBYAK=on
)

vcpkg_install_cmake()
vcpkg_cmake_config_fixup(PACKAGE_NAME CommonLibSSE CONFIG_PATH lib/cmake)
vcpkg_copy_pdbs()

file(GLOB CMAKE_CONFIGS "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE/*.cmake")
file(INSTALL ${CMAKE_CONFIGS} DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")
file(INSTALL "${SOURCE_PATH}/cmake/CommonLibSSE.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE")

file(
        INSTALL "${SOURCE_PATH}/LICENSE"
        DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
        RENAME copyright)

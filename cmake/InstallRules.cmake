if(NOT ZYX_WASM AND TARGET zyx)
    install(TARGETS zyx
        EXPORT zyxTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/zyx/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/zyx"
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
    )

    install(EXPORT zyxTargets
        NAMESPACE zyx::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zyx"
    )

    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/zyxConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/zyxConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zyx"
    )
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/zyxConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${PROJECT_BINARY_DIR}/zyxConfig.cmake"
        "${PROJECT_BINARY_DIR}/zyxConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zyx"
    )

    if(ZYX_INSTALL_PKGCONFIG)
        configure_file(
            "${PROJECT_SOURCE_DIR}/cmake/zyx.pc.in"
            "${PROJECT_BINARY_DIR}/zyx.pc"
            @ONLY
        )
        install(FILES "${PROJECT_BINARY_DIR}/zyx.pc"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        )
    endif()
endif()

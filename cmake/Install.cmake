include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set_target_properties(tgpp PROPERTIES EXPORT_NAME tgpp)
set_target_properties(tgpp_ext PROPERTIES EXPORT_NAME ext)
set(TGPP_INSTALL_TARGETS tgpp tgpp_ext)
foreach(component tgpp_asio tgpp_async tgpp_async_ext tgpp_sqlite tgpp_llhttp tgpp_webhook)
    if(TARGET ${component})
        list(APPEND TGPP_INSTALL_TARGETS ${component})
    endif()
endforeach()
install(
    TARGETS ${TGPP_INSTALL_TARGETS}
    EXPORT tgppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/tgpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TGPP_BUILD_ASYNC)
    install(DIRECTORY "${TGPP_ASIO_INCLUDE_DIR}/asio" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES "${TGPP_ASIO_INCLUDE_DIR}/asio.hpp" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
configure_package_config_file(
    cmake/tgppConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/tgppConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tgpp
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/tgppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tgppConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/tgppConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tgpp
)
install(
    EXPORT tgppTargets
    NAMESPACE tgpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tgpp
)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tgpp)
if(TGPP_BUILD_ASYNC AND EXISTS "${tgpp_asio_source_SOURCE_DIR}/asio/LICENSE_1_0.txt")
    install(FILES "${tgpp_asio_source_SOURCE_DIR}/asio/LICENSE_1_0.txt"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tgpp/asio
    )
endif()
if(TGPP_WITH_WEBHOOK)
    install(FILES "${tgpp_llhttp_source_SOURCE_DIR}/LICENSE"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tgpp/llhttp
    )
endif()

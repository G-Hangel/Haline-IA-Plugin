set(kritatoolhaline_SOURCES
    tool_haline.cc
    kis_tool_haline.cc
    )

qt_add_resources(kritatoolhaline_SOURCES haline.qrc)

kis_add_library(kritatoolhaline MODULE ${kritatoolhaline_SOURCES})

target_link_libraries(kritatoolhaline kritaui)

install(TARGETS kritatoolhaline DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

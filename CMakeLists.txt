if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
  # Firmware owns its version independently of the desktop application.
  file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" LUMI_RELEASE_VERSION)
  string(STRIP "${LUMI_RELEASE_VERSION}" LUMI_RELEASE_VERSION)
  if(NOT LUMI_RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a firmware version in X.Y.Z format")
  endif()
  message(STATUS "RYNOR ONE firmware version: ${LUMI_RELEASE_VERSION}")
  target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
  )
  target_compile_definitions(
    app PRIVATE
    LUMI_FIRMWARE_VERSION=\"${LUMI_RELEASE_VERSION}\"
  )

  target_sources(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/lumi_panel.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lumi_status_screen.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lumi_battery.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lumi_rgb.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lumi_action.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lumi_now_playing.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lumi_app_link.c
  )
endif()

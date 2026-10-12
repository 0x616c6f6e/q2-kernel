# LVGL's portable C software renderer; no upstream platform integration or window drivers are enabled.
file(GLOB_RECURSE Q2_LVGL_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../third_party/lvgl/src/*.c")
add_library(q2_lvgl_vendor STATIC ${Q2_LVGL_SOURCES})
target_include_directories(q2_lvgl_vendor PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/lvgl"
    "${CMAKE_CURRENT_LIST_DIR}/../components/display")
target_compile_definitions(q2_lvgl_vendor PUBLIC LV_CONF_INCLUDE_SIMPLE=1
    Q2_ENABLE_CJK_FONT=$<BOOL:${Q2_ENABLE_CJK_FONT}>)
if(UNIX)
  target_link_libraries(q2_lvgl_vendor PUBLIC m)
endif()

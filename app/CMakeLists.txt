target_sources(app PRIVATE
    src/main.c
    src/microbench.c
    src/draw_nano2d/lv_draw_nano2d.c
)

# Use LVGL's internal idle measurement for CPU (CONFIG_CPU_LOAD unavailable on A7D65 for some reason)
zephyr_compile_definitions(LV_SYSMON_GET_IDLE=lv_timer_get_idle)

# LVGL private headers (lv_draw_private.h, lv_area_private.h) for the nano2D draw
# unit, the nano2D GPU public headers, and the LVGL demo sources.
set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})
target_include_directories(app PRIVATE
    ${LVGL_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/gpu/microchip_gc520ul/inc
)
file(GLOB_RECURSE LVGL_DEMO_SRCS
    ${LVGL_DIR}/demos/benchmark/*.c
    ${LVGL_DIR}/demos/widgets/*.c
    ${LVGL_DIR}/demos/lv_demos.c
)
target_sources(app PRIVATE ${LVGL_DEMO_SRCS})

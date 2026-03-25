# board: sama7d65_curiosity/sama7d65
cmake_minimum_required(VERSION 3.20.0)

# 1. Register the current directory as a Zephyr Module
set(ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR})

# 2. Tell Zephyr where to find the application config files
set(CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/app/prj.conf)
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/app/app.overlay)

# 3. Initialize Zephyr
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(lcdc_poc)

# 4. Add the application source logic
add_subdirectory(app)

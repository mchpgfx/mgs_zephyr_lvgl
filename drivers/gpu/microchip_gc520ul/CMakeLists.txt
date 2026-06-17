# GC520UL GPU driver: consumer-side glue + prebuilt vendor library.
#
# The proprietary nano2D core is linked as a prebuilt static archive
# (lib/libgpu2dc.a) built separately from the nano2d repo
# (projects/gc520ul/standalone/build.sh) and copied here manually along with
# its public headers (inc/). There is intentionally NO build dependency on the
# GPU source tree. The .a's external refs resolve at final link:
#   - libc (printf/memcpy/...) -> picolibc
#   - n2d_hal_* hooks          -> microchip_gc520ul_hal.c (below)

# Consumer glue: device driver + HAL implementation (compiled from source).
zephyr_library_sources_ifdef(CONFIG_GPU_MICROCHIP_GC520UL
	microchip_gc520ul.c
	microchip_gc520ul_hal.c)

if(CONFIG_GPU_MICROCHIP_GC520UL)
	# Vendor public headers only (incl. nano2D_hal.h - the HAL contract).
	# Internal register/ioctl headers (inc/shared) are not shipped.
	zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/inc)

	# Prebuilt proprietary core.
	zephyr_library_import(gpu2dc ${CMAKE_CURRENT_LIST_DIR}/lib/libgpu2dc.a)
endif()

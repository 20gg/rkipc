
#include(FindPkgConfig)
#pkg_check_modules (GLIB REQUIRED IMPORTED_TARGET glib-2.0)

# link_directories(${PROJECT_SOURCE_DIR}/lib)
include_directories(${PROJECT_SOURCE_DIR}/include
					${CMAKE_SYSROOT}/usr/include/rkaiq/
					${CMAKE_SYSROOT}/usr/include/rkaiq/isp
					${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI
					${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI2
					${CMAKE_SYSROOT}/usr/include/rkaiq/algos
					${CMAKE_SYSROOT}/usr/include/rkaiq/common
					${CMAKE_SYSROOT}/usr/include/rkaiq/xcore
					${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser
					${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser_v2
					${CMAKE_SYSROOT}/usr/include/libdrm
					${CMAKE_SYSROOT}/usr/include/freetype2
					${CMAKE_SYSROOT}/usr/include/lvgl
					${WPA_SUPPLICANT_INCLUDE_DIRS}
					)

aux_source_directory(./ SRCS)

aux_source_directory(video SRCS)
aux_source_directory(ui SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/ui/common SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/lvgl/common SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/lvgl/drivers SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/lvgl/porting SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/ SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/audio SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/isp/rk3576 SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/socket_server SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/param SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/system SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rtsp SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rtmp SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/osd SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/osd/freetype2 SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/roi SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/network SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/storage SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/event SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/region_clip SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/sysutil SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/player SRCS)

include_directories(video
					.
					${PROJECT_SOURCE_DIR}/common
					${PROJECT_SOURCE_DIR}/common/player
					${PROJECT_SOURCE_DIR}/common/ui/common
					${PROJECT_SOURCE_DIR}/common/lvgl/common
					${PROJECT_SOURCE_DIR}/common/lvgl/drivers
					${PROJECT_SOURCE_DIR}/common/lvgl/porting
					${PROJECT_SOURCE_DIR}/common/audio
					${PROJECT_SOURCE_DIR}/common/isp/rk3576
					${PROJECT_SOURCE_DIR}/common/socket_server
					${PROJECT_SOURCE_DIR}/common/rtsp
					${PROJECT_SOURCE_DIR}/common/rtmp
					${PROJECT_SOURCE_DIR}/common/param
					${PROJECT_SOURCE_DIR}/common/system
					${PROJECT_SOURCE_DIR}/common/osd
					${PROJECT_SOURCE_DIR}/common/osd/freetype2
					${PROJECT_SOURCE_DIR}/common/roi
					${PROJECT_SOURCE_DIR}/common/network
					${PROJECT_SOURCE_DIR}/common/storage
					${PROJECT_SOURCE_DIR}/common/event
					${PROJECT_SOURCE_DIR}/common/rockiva
					${PROJECT_SOURCE_DIR}/common/region_clip
					${PROJECT_SOURCE_DIR}/common/sysutil
					)

link_directories(${PROJECT_SOURCE_DIR}/lib/aarch64-rockchip1031-linux-gnu)

add_definitions(-DISP_HW_V39)
add_definitions(-g -ggdb)
add_definitions(-DDRAW_UI_BY_VO=1 -DLVGL_SUPPORT_KEY)

add_executable(${PROJECT_NAME} ${SRCS})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-result -Wno-unused-but-set-variable -Wno-unused-variable")

target_link_libraries(${PROJECT_NAME}
		pthread rockit rkaiq rtsp freetype wpa_client rkmuxer m rga lvgl png drm rkdemuxer rkaudio)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
file(GLOB RKIPC_INI_FILES rkipc-*)
install(FILES ${RKIPC_INI_FILES} DESTINATION share)
file(GLOB EIS_CONFIG_FILES video/rkeis_config*)
install(FILES ${EIS_CONFIG_FILES} DESTINATION share)
install(PROGRAMS RkLunch.sh DESTINATION bin)
install(PROGRAMS RkLunch-stop.sh DESTINATION bin)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/PuHuiTi.ttf DESTINATION share)
install(FILES asound.conf DESTINATION etc)

file(GLOB ICON_FILES res/ui/*)
install(FILES ${ICON_FILES} DESTINATION share/res/ui/)

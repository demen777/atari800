# USB HID keyboard host, translated to XT set-1 scancodes for
# handleScancode(). Only added as a subdirectory when KBD_USB is on.
#
# host_cfg/ carries the TinyUSB configuration and is exported here rather than
# from the top-level include list: putting a host tusb_config.h on an
# unconditional -I is what once left the firmware with no device CDC.

add_library(usbkbd INTERFACE)

target_sources(usbkbd INTERFACE
	${CMAKE_CURRENT_LIST_DIR}/usbkbd.c
)

target_include_directories(usbkbd INTERFACE
	${CMAKE_CURRENT_LIST_DIR}
	${CMAKE_CURRENT_LIST_DIR}/host_cfg
)

target_link_libraries(usbkbd INTERFACE
	tinyusb_host
	tinyusb_board
)

add_library(hdmi INTERFACE)

# hstx_packet.c is the packet layer of https://github.com/fliperama86/pico_hdmi
# (commit baf8ac37, Unlicense), vendored unmodified under drivers/pico_hdmi with
# the two headers it pulls in. It builds HDMI packets and TERC4-encodes them;
# hdmi_audio.c feeds the result to the PIO driver. The rest of that library
# drives the HSTX peripheral, which is fixed to GPIO12-19 and cannot reach this
# board's DVI connector, so it is deliberately not here.
target_sources(hdmi INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/hdmi.c
        ${CMAKE_CURRENT_LIST_DIR}/hdmi_audio.c
        ${CMAKE_CURRENT_LIST_DIR}/../pico_hdmi/src/hstx_packet.c
)

target_link_libraries(hdmi INTERFACE hardware_pio hardware_clocks hardware_dma)

target_include_directories(hdmi INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../pico_hdmi/include
)

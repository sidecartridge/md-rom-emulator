# Create a library for the httpc files
add_library(httpc
    httpc.c
)

# Add the include directory (for httpc.h)
target_include_directories(httpc PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..   # add parent folder if lwipopts.h is there
)

# Link necessary libraries
target_link_libraries(httpc
    pico_stdlib    # Core Pico SDK library
    pico_cyw43_arch_lwip_threadsafe_background 
    pico_lwip_http
    pico_lwip_mbedtls
    pico_mbedtls
)

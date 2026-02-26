# Check if WIFI_UTILITIES is set in Kconfig
if(CONFIG_WIFI_UTILITIES)

    # Add your source file directory
    zephyr_include_directories(.)

    # Add the source file you want to compile
    zephyr_library_sources(wifi_utilities.c)

endif()
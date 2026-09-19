idf_component_register(
    SRCS
        "main.c"
        "gias_system.c"
        "i2s_audio.c"
        "sdmmc_manager.c"
        "rtc_wifi.c"
        "led.c"
        "power.c"
       

    INCLUDE_DIRS
        "."

    REQUIRES
        driver
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        fatfs
        sdmmc
)
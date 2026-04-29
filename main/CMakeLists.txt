idf_component_register(
    SRCS
        "pacman_main.c"
        "pacman_game.c"
        "lcd_driver.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp_driver_spi esp_driver_gpio nvs_flash
)

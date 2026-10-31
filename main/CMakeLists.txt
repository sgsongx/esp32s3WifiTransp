idf_component_register(SRCS "softap_example_main.c"
                    PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_driver_uart lwip
                    INCLUDE_DIRS ".")

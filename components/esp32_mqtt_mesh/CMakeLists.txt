idf_component_register(SRCS "esp32_mqtt_mesh.c" "mqtt_app.c" "mesh_netif.c"
                        INCLUDE_DIRS "."
                        PRIV_INCLUDE_DIRS ""
                        PRIV_REQUIRES "esp-tls" "nvs_flash" "mqtt"
                        REQUIRES   "esp_wifi")

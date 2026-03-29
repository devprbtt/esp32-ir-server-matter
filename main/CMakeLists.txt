idf_component_register(SRCS "app_main.cpp" "ir_sender.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_rmt ds18b20)

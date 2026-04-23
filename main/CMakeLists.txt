idf_component_register(SRCS "app_main.cpp" "ir_sender.cpp"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "certs/tls_root_ca.pem"
                       REQUIRES esp_driver_rmt ds18b20 esp_event esp_netif mdns)

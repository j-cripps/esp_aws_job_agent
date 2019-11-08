# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

idf_component_register(SRCS "ma_jobAgent.c" "ma_jobOTA.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "../esp-aws-iot/port/include" "../esp-aws-iot/aws-iot-device-sdk-embedded-C/include" "$ENV{IDF_PATH}/components"
                       REQUIRES esp-aws-iot jsmn nvs_flash esp_https_ota app_update mbedtls)
                       
if(CONFIG_EXAMPLE_EMBEDDED_CERTS)
target_add_binary_data(${COMPONENT_TARGET} "../../certs/aws-root-ca.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "../../certs/certificate.pem.crt" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "../../certs/private.pem.key" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "../../certs/s3_root_cert.pem" TEXT)
endif()

/**
 * @file ma_jobOTA.h
 *
 * @brief Contains functions relating to the OTA (over-the-air) update feature
 *
 * @author Jack Cripps
 * @date 25/10/2019
 */

#ifndef __ma_jobOTA__
#define __ma_jobOTA__

#include "esp_err.h"

/* Enums and Structs
 * -------------------------------------------------------------------------- */
/**
 * @brief Return type for the https OTA update function
 */
typedef enum otaReturn {
    MA_OTA_HTTP_ERR = -5,   /**< Error caused by the http client itself, or underlying TLS, TCP/IP stacks */
    MA_OTA_JOB_ERR = -4,    /**< Error caused by AWS Job, for example Job version discrepancy */
    MA_OTA_DUP_FW = -3,     /**< Error due to new firmware and current firmware having same version */
    MA_OTA_INV_FW = -2,     /**< Error caused by a new firmware version matching a previously invalid version */
    MA_OTA_ERR = -1,        /**< Generic error message */
    MA_OTA_SUCCESS = 0      /**< OTA update via https successful */
} ma_ota_err_t;


/* Function Prototypes
 * -------------------------------------------------------------------------- */
/**
 * @brief   Function to perform an OTA firmware update via HTTPS
 *
 * @param[in]   appVersion  The version string contained within the AWS job
 * @param[in]   url         The url of new FW contained within the AWS job
 * @param[in]   serverCertPemStart  The starting byte of the public cert for the url
 *
 * @return
 *      - MA_OTA_HTTP_ERR
 *      - MA_OTA_JOB_ERR
 *      - MA_OTA_DUP_FW
 *      - MA_OTA_INV_FW
 *      - MA_OTA_ERR
 *      - MA_OTA_SUCCESS
 */
ma_ota_err_t httpsOtaUpdate(const char* appVersion, const char* url, const uint8_t* serverCertPemStart);


#endif /* __ma_jobOTA__ */

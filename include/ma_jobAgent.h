/**
 * @file jobEngine.h
 *
 * @brief Interacts with the AWS Jobs API to pull any relevant jobs for this Thing, parse them and store the job with any relevant info
 * inside a struct that needs to be dealt with by the rest of the application
 *
 * @author Jack Cripps
 * @date 11/09/2019
 */

#ifndef __ma_jobAgent__
#define __ma_jobAgent__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_event_loop.h"

/* Public Event Group Bits
 * -------------------------------------------------------------------------- */
/**
 * @brief   Event group to signal when we are connected & ready to make a request
 */
EventGroupHandle_t jobAgentEventGroup;

static const int WIFI_PROVISIONED_BIT = BIT0;      /**< Whether the task has been provided with WiFi credentials */
static const int WIFI_CONNECTED_BIT = BIT1;        /**< Whether WiFi stack is connected */
static const int JOB_OTA_RESTART_BIT = BIT2;       /**< Whether task requests restart of unit, usually for OTA update */
static const int WORK_COMPLETED_BIT = BIT3;        /**< Task completed all work so can be stopped */
static const int JOB_OTA_REQUEST_BIT = BIT4;       /**< Signifies an OTA job has been received to the main app */
static const int JOB_OTA_REPLY_BIT = BIT5;         /**< OTA update will not proceed unless this bit has been set by main app */


/* Public Function Prototypes
 * -------------------------------------------------------------------------- */

void job_agent_task(void *param);



#endif /* __ma_jobAgent__ */

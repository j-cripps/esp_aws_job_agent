/**
 * @file ma_jobAgent.c
 *
 * @brief Interacts with the AWS Jobs API to pull any relevant jobs for this Thing,
 * parse them and execute the job.
 *
 * @author Jack Cripps // jackc@monitoraudio.com
 * @date 11/09/2019
 */


#include "ma_jobAgent.h"

#include "ma_jobOTA.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "jsmn.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"
#include "aws_iot_jobs_interface.h"


/* -------------------------------------------------------------------------- */
/**
 * @brief   Macro for compile time array sizing
 */
#define COUNT_OF(x) ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))


/* -------------------------------------------------------------------------- */
/**
 * @brief   WiFi SSID and pass provided here as defines, before being implemented to be sent into task
 *          during WiFi provisioning step
 */
#define EXAMPLE_WIFI_SSID "Monitor Audio WiFi"
#define EXAMPLE_WIFI_PASS "des1gnf0rs0und"


/* -------------------------------------------------------------------------- */
/**
 * @brief   A job as a structure
 */
typedef struct jobStruct {
    char jobId[32];     /**< ID of the job obtained from AWS jobs service */
    char jobDoc[256];   /**< The job document obtained from AWS jobs service */
    char jobFailMsg[256];   /**< If a job fails, store failure message in here to send back to AWS */
    bool inProgress;    /**< Flag to indicate whether a job came down from AWS as in progress or not(queued) */
} jobDefinition_t;

/* -------------------------------------------------------------------------- */


static const char *TAG = "ma_JobAgent"; /**< ESP-IDF logging file name */

const char *THING_NAME = "ESP32TestThing";  /**< The unique name of this unit, used by AWS, temporarily here but should be stored in flash at unit production time */

/**
 * @brief   Event group to signal when we are connected & ready to make a request
 */
EventGroupHandle_t taskEventGroup;

const int WIFI_PROVISIONED_BIT = BIT0;      /**< Whether the task has been provided with WiFi credentials */
const int WIFI_CONNECTED_BIT = BIT1;        /**< Whether WiFi stack is connected */
const int RESTART_REQUESTED_BIT = BIT2;     /**< Whether task requests restart of unit (usually after successful OTA update) */
const int WORK_COMPLETED_BIT = BIT3;        /**< Task completed all work so can be stopped */
const int JOB_READY_BIT = BIT4;             /**< Whether job ready to be processed */
const int JOB_PROCESSING_BIT = BIT5;        /**< Whether an AWS job is currently being processed */
const int JOB_COMPLETED_BIT = BIT6;         /**< AWS job stage completed flag */
const int JOB_FAILED_BIT = BIT7;            /**< Generic job failed flag */
const int UPDATE_FAILED_BIT = BIT8;         /**< Software update failed flag */


/**
 * @brief   - CA Root certificate
 *          - device ("Thing") certificate
 *          - device ("Thing") key
 *          - s3 root cert for connecting to AWS S3
 */
extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");
extern const uint8_t s3_root_cert_start[] asm("_binary_s3_root_cert_pem_start");
extern const uint8_t s3_root_cert_end[] asm("_binary_s3_root_cert_pem_end");

/**
 * @brief   Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
char HostAddress[255] = AWS_IOT_MQTT_HOST;

/**
 * @brief   Default MQTT port is pulled from the aws_iot_config.h
 */
uint32_t port = AWS_IOT_MQTT_PORT;

/* AWS Client Information */
static AWS_IoT_Client client;
static IoT_Client_Init_Params mqttInitParams;
static IoT_Client_Connect_Params connectParams;
static IoT_Publish_Message_Params paramsQOS0;

/* AWS test variables */
char testPayload[32];
const char *TEST_TOPIC = "test_topic/esp32";
const uint8_t TEST_TOPIC_LEN = strlen("test_topic/esp32");
int32_t testVar = 0;

/* AWS Topic and Storage Buffers */
static char getRejectedSubBuf[128];
static char getAcceptedSubBuf[128];
static char getJobAcceptedSubBuf[128];
static char getJobUpdateSubBuf[128];
static char getJobUpdateAcceptedSubBuf[128];
static char getJobUpdateRejectedSubBuf[128];
static char tempJobBuf[128];
static char tempStringBuf[512];

/* Job storage */
static jobDefinition_t currentJob;

/* Jsmn JSON Parser */
static jsmn_parser jParser;
static jsmntok_t jTokens[128];

/* Private Function Prototypes
 * -------------------------------------------------------------------------- */
/**
 * @brief   Function when an unrecoverable error has occurred. Task must be terminated at this point
 */
static void taskFatalError(void);

/**
 * @brief   The event handler callback listening for WiFi stack events
 *
 * @param   ctx[in]   Event context
 * @param   event[in] The event - to check if WiFi connect/disconnect event etc
 *
 * @return
 *      - ESP_OK
 *      - ESP_FAIL
 */
static esp_err_t event_handler(void *ctx, system_event_t *event);

/**
 * @brief   Pull out a JSON token parsed by JSMN as a string
 *
 * @param   pToken[in]    Pointer to the JSMN token to extract as a string
 * @param   source[in]    The source JSON file buffer which was parsed by JSMN
 * @param   dest[out]     An output buffer to store the extracted string
 * @param   destLen[in]   Length of dest buffer including null termination space
 *
 * @return  Status of extraction, can fail due to token being NULL or destination being too small
 */
static bool extractJsonTokenAsString(const jsmntok_t *pToken, const char *source, char *dest, uint16_t destLen);

/**
 * @brief   Take a job failure message and format it into a valid status details JSON object for AWS
 *
 * @param[in]   errMessage  The error message, usually stored as partof the current jobobject
 * @param[out]  destBuf     A destination buffer to store the formatted string
 * @param[out]  destBufLen  Length of the destination buffer in bytes
 *
 * @return  Status of function, failure indicates destination buffer too small to fit message
 */
static bool formatJobErrorStatusDetails(const char *errMessage, char *destBuf, uint16_t destBufLen);

/**
 * @brief   Check whether the WiFi credentials are present and valid (i.e provisioned)
 *
 * @note    Currently always returns true until WiFi provisioning functionality set up the
 *          WiFi details are defined at the top of the file
 *
 * @return  Status of provisioning, false when not provisioned or provisioned details are not valid
 */
static bool wifiProvisionedCheck(void);

/**
 * @brief   Initialise and connect the WiFi stack - waits on being provisioned before proceeding with this function
 */
static void initialiseWifi(void);

/**
 * @brief   Firmware diagnostic test to check if the app just booted is valid
 *
 * @note    Not currently implemented - Always returns true
 *
 * @return  Status of diagnostic test
 */
static bool diagnostic(void);

/**
 * @brief   Responsible for running the diagnostic test and checking whether the app that has booted is valid. If not
 *          then rollback the app to a previously valid version, else mark app as valid and continue with normal
 *          running of the program
 */
static void bootValidityCheck(void);

/**
 * @brief   Check for NVS errors upon boot, this may not be able to be called in production as could erase flash
 *
 * @return  Success of the check, or one of underlying flash errors if failed
 */
static esp_err_t nvsBootCheck(void);

/**
 * @brief   Check if OTA partitions are configured correctly upon boot
 */
static void otaBootCheck(void);

/**
 * @brief   Callback for when disconnect event from AWS occurs
 */
static void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data);

/**
 * @brief   Callback for when get pending jobs request from AWS is rejected
 */
static void awsGetRejectedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                          IoT_Publish_Message_Params *params, void *pData);
/**
 * @brief   Callback for when get pending jobs request from AWS is accepted
 *
 * @note    This is where a list of in progress and queued jobs are gathered from AWS.
 *          - In progress jobs are always processed first and in order as these are usually to do with jobs that
 *            required SW restart such as OTA updates, and so validity of the update must be established before
 *            proceeding
 *
 *          - Queued jobs could be processed in any order, but currently processed in order dictated by AWS
 *
 */
static void awsGetAcceptedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                          IoT_Publish_Message_Params *params, void *pData);
/**
 * @brief   Callback for when job description request to AWS is accepted
 *
 * @note    This is where the job execution doc and job doc are parsed to find out job type and details
 */
static void awsJobGetAcceptedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                             IoT_Publish_Message_Params *params, void *pData);
/**
 * @brief   Callback for a specific job update message, not currently used, instead using job agnostic callback
 *          functions below as this level of specificity is unnecessary
 */
static void awsGetJobUpdateCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                           IoT_Publish_Message_Params *params, void *pData);
/**
 * @brief   Callback for AWS job update response if the update request (IN_PROGRESS, FAILED etc) is accepted
 */
static void awsUpdateAcceptedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                             IoT_Publish_Message_Params *params, void *pData);
/**
 * @brief   Callback for AWS job update response if the update request (IN_PROGRESS, FAILED etc) is rejected
 */
static void awsUpdateRejectedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                             IoT_Publish_Message_Params *params, void *pData);
/**
 * @brief   Once all the details (job doc) of a job is obtained from AWS we can begin executing it, whether in progress
 *          job or a new queued job
 */
static void awsProcessJob(void);

/**
 * @brief   Initialise and connect to AWS jobs service. Subscribes to AWS jobs services.
 *
 * @note    Currently treat every connect attempt as a fresh session, meaning if accidental disconnect occurs the unit
 *          does not attempt to subscribe twice to topics as this is undefined behaviour
 */
static void connectToAWS(void);

/**
 * @brief   Unsubscribe and disconnect from AWS before exiting this task or requesting a restart, or setting work completed bit.
 */
static void unsubAndDisconnnectAWS(void);


/* Function Definitions
 * -------------------------------------------------------------------------- */
static void taskFatalError(void)
{
    while (1)
    {
        ESP_LOGE(TAG, "Fatal error in task, no return");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    (void)vTaskDelete(NULL);
}


static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(taskEventGroup, WIFI_CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(taskEventGroup, WIFI_CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}


static bool extractJsonTokenAsString(const jsmntok_t *pToken, const char *source, char *dest, uint16_t destLen)
{
    /* Don't copy if token is null (i.e token could not be found in json document) */
    if (pToken == NULL)
    {
        return false;
    }

	uint16_t sourceLen = pToken->end - pToken->start;

	/* Don't copy if no room in dest buffer for whole string + null terminator */
	if (sourceLen + 1 > destLen)
	{
		return false;
	}
	else
	{
		/* Copy from start of token in buffer over to destination*/
		memcpy(dest, &source[pToken->start], sourceLen);
		/* Append null terminator */
		dest[sourceLen] = '\0';
	}

	return true;
}


static bool formatJobErrorStatusDetails(const char *errMessage, char *destBuf, uint16_t destBufLen)
{
    if (errMessage == NULL || destBuf == NULL || destBufLen < 10 || (strlen(errMessage) + 10 > destBufLen))
    {
        ESP_LOGE(TAG, "Error formatting job error status");
        return false;
    }

    int n = snprintf(destBuf, destBufLen, "{\"errMessage\":\"%s\"}", errMessage);
    if (n < 0 || n > destBufLen)
    {
        ESP_LOGE(TAG, "Error formatting job error status");
        return false;
    }

    return true;
}


static bool wifiProvisionedCheck(void)
{
	bool ret = true;



	return ret;
}


static void initialiseWifi(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}


static bool diagnostic(void)
{
    ESP_LOGI(TAG, "Running FW image validation");
    /* This needs implementing
     * - Need to come up with test to check if app is valid
     * - Current implementation always passes
     */

    return true;
}


static void bootValidityCheck(void)
{
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_img_states_t ota_state;
	if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK)
	{
		if (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
		{
			/* Run the diagnostic function */
			bool diagnostic_ok = diagnostic();
			if (diagnostic_ok)
			{
				ESP_LOGI(TAG, "Diagnostics completed successfully");
				esp_ota_mark_app_valid_cancel_rollback();
				// Notify AWS that job completed successfully
			}
			else
			{
				ESP_LOGE(TAG, "Diagnostics failed, rolling back to previous version");
				esp_ota_mark_app_invalid_rollback_and_reboot();
			}
		}
	}
}


static esp_err_t nvsBootCheck(void)
{
	/* Initialise NVS */
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		/* OTA app partition table has a smaller NVS partition size than the non-OTA
		 * partition table. This size mismatch may cause NVS initialisation to fail.
		 * If this happens, we erase NVS partition and initialise NVS again. */
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	return err;
}


static void otaBootCheck(void)
{
	const esp_partition_t *configured = esp_ota_get_boot_partition();
	const esp_partition_t *running = esp_ota_get_running_partition();

	if (configured != running)
	{
		ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
				 configured->address, running->address);
		ESP_LOGW(TAG, "This can happen if either the OTA boot data or preferred boot image becomes corrupted.");
	}

	ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
			 running->type, running->subtype, running->address);
}


void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data)
{
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if (NULL == pClient)
    {
    	ESP_LOGE(TAG, "MQTT Disconnect, AWS client NULL");
        return;
    }

    if (aws_iot_is_autoreconnect_enabled(pClient))
    {
        ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    }
    else
    {
        ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if (NETWORK_RECONNECTED == rc)
        {
            ESP_LOGW(TAG, "Manual Reconnect Successful");
        }
        else
        {
            ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}


void awsGetRejectedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                   IoT_Publish_Message_Params *params, void *pData)
{
	ESP_LOGW(TAG, "\n Rejected: %.*s\t%.*s\n", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);

	/* Check if a message has been received with any data inside */
	if ((params->payloadLen) <= 0)
	{
		ESP_LOGW(TAG, "Subscription Callback: Message payload has length of 0");
	}
	else
	{

	}
}


void awsGetAcceptedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                   IoT_Publish_Message_Params *params, void *pData)
{
	/* ESP_LOGI(TAG, "\n Accepted: %.*s\t%.*s\n", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload); */
	bool ret = false;

	/* Check if a message has been received with any data inside */
	if ((params->payloadLen) <= 0)
	{
		ESP_LOGW(TAG, "Subscription Callback: Message payload has length of 0");
	}
	else
	{
		/* Initialise jsmn parser and check it is valid */
		jsmn_init(&jParser);
		int r = jsmn_parse(&jParser, params->payload, params->payloadLen, jTokens, COUNT_OF(jTokens));

		/* Check if parsed json is valid */
		if (r < 0)
		{
			ESP_LOGE(TAG, "Job Accepted Callback: Failed to parse JSON: %d", r);
			taskFatalError();
		}

		if ((r < 1) || (jTokens[0].type != JSMN_OBJECT))
		{
			ESP_LOGE(TAG, "Job Accepted Callback: Expected object");
			taskFatalError();
		}

		/* Search through JSON for in progress jobs, as these need dealing with first */
		jsmntok_t *inProgressToken = findToken("inProgressJobs", params->payload, &jTokens[0]);
		if (inProgressToken != NULL)
		{
			if (inProgressToken->size == 0)
			{
				ESP_LOGI(TAG, "No in progress jobs");
			}
			else
			{
			    xEventGroupSetBits(taskEventGroup, JOB_PROCESSING_BIT);

				ret = extractJsonTokenAsString(inProgressToken, (char *)params->payload, &tempStringBuf[0], COUNT_OF(tempStringBuf));
				if (!ret)
				{
					ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
					taskFatalError();
				}
				ESP_LOGI(TAG, "Size: %d, In progress jobs: %s", inProgressToken->size, tempStringBuf);

				currentJob.inProgress = true;

				jsmntok_t *jobToken = inProgressToken;

				while (jobToken->type != JSMN_OBJECT)
                {
                    jobToken++;
                }
				/* Job found */
                ret = extractJsonTokenAsString(jobToken, (char *)params->payload, &tempStringBuf[0], COUNT_OF(tempStringBuf));
                if (!ret)
                {
                    ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
                    taskFatalError();
                }
                ESP_LOGI(TAG, "Size: %d, Job: %s", jobToken->size, tempStringBuf);

                jsmntok_t *jobIdToken = findToken("jobId", params->payload, jobToken);
                ret = extractJsonTokenAsString(jobIdToken, (char *)params->payload, &currentJob.jobId[0], COUNT_OF(currentJob.jobId));
                if (!ret)
                {
                    ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
                    taskFatalError();
                }
                // ESP_LOGI(TAG, "Job Id: %s", currentJob.jobId);

                /* Trigger job execution by asking AWS for job document.
                 * - Job execution takes place in awsJobGetAcceptedCallback */
                AwsIotDescribeJobExecutionRequest describeRequest;
                describeRequest.executionNumber = 0;
                describeRequest.includeJobDocument = true;
                describeRequest.clientToken = NULL;
                IoT_Error_t rc = aws_iot_jobs_describe(pClient, QOS0, THING_NAME, currentJob.jobId, &describeRequest,
                                                       tempJobBuf, COUNT_OF(tempJobBuf), NULL, 0);
                if (SUCCESS != rc)
                {
                    ESP_LOGE(TAG, "Unable to publish job description request: %d", rc);
                    taskFatalError();
                }
			}
		}
		else
		{
			ESP_LOGE(TAG, "Token not found");
		}

		/* Search through JSON for queued jobs */
		jsmntok_t *queuedToken = findToken("queuedJobs", params->payload, &jTokens[0]);
		if (queuedToken != NULL)
		{
			if (queuedToken->size == 0)
			{
				ESP_LOGI(TAG, "No queued jobs");
			}
			else
			{
			    xEventGroupSetBits(taskEventGroup, JOB_PROCESSING_BIT);

				ret = extractJsonTokenAsString(queuedToken, (char *)params->payload, &tempStringBuf[0], COUNT_OF(tempStringBuf));
				if (!ret)
				{
					ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
					taskFatalError();
				}
				ESP_LOGI(TAG, "Size: %d, Queued jobs: %s", queuedToken->size, tempStringBuf);

				currentJob.inProgress = false;

				/* Currently work through buffer from first job object */

				/* Since jobs are stored as objects within an array, search forwards to find next object(job) */
				jsmntok_t *jobToken = queuedToken;

				while (jobToken->type != JSMN_OBJECT)
				{
					jobToken++;
				}

				/* Job found */
				ret = extractJsonTokenAsString(jobToken, (char *)params->payload, &tempStringBuf[0], COUNT_OF(tempStringBuf));
				if (!ret)
				{
					ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
					taskFatalError();
				}
				ESP_LOGI(TAG, "Size: %d, Job: %s", jobToken->size, tempStringBuf);

				jsmntok_t *jobIdToken = findToken("jobId", params->payload, jobToken);
				ret = extractJsonTokenAsString(jobIdToken, (char *)params->payload, &currentJob.jobId[0], COUNT_OF(currentJob.jobId));
				if (!ret)
				{
					ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
					taskFatalError();
				}
				// ESP_LOGI(TAG, "Job Id: %s", currentJob.jobId);

				/* Trigger job execution by asking AWS for job document.
				 * - Job execution takes place in awsJobGetAcceptedCallback */
				AwsIotDescribeJobExecutionRequest describeRequest;
				describeRequest.executionNumber = 0;
				describeRequest.includeJobDocument = true;
				describeRequest.clientToken = NULL;
				IoT_Error_t rc = aws_iot_jobs_describe(pClient, QOS0, THING_NAME, currentJob.jobId, &describeRequest,
													   tempJobBuf, COUNT_OF(tempJobBuf), NULL, 0);
				if (SUCCESS != rc)
				{
					ESP_LOGE(TAG, "Unable to publish job description request: %d", rc);
					taskFatalError();
				}
			}
		}
		else
		{
			ESP_LOGE(TAG, "Token not found");
		}
	}
}


void awsJobGetAcceptedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                   IoT_Publish_Message_Params *params, void *pData)
{
	/* ESP_LOGI(TAG, "\n Job Accepted: %.*s\t%.*s\n", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload); */
	bool ret  = false;

	/* Check if a message has been received with any data inside */
	if ((params->payloadLen) <= 0)
	{
		ESP_LOGW(TAG, "Subscription Callback: Message payload has length of 0");
	}
	else
	{
		/* Initialise jsmn parser and check it is valid */
		jsmn_init(&jParser);
		int r = jsmn_parse(&jParser, params->payload, params->payloadLen, jTokens, COUNT_OF(jTokens));

		/* Check if parsed json is valid */
		if (r < 0)
		{
			ESP_LOGE(TAG, "Job Accepted Callback: Failed to parse JSON: %d", r);
			taskFatalError();
		}

		if ((r < 1) || (jTokens[0].type != JSMN_OBJECT))
		{
			ESP_LOGE(TAG, "Job Accepted Callback: Expected object");
			taskFatalError();
		}

		/* First must find execution document */
		jsmntok_t *execDocToken = findToken("execution", params->payload, &jTokens[0]);
		ret = extractJsonTokenAsString(execDocToken, (char *)params->payload, &tempStringBuf[0], COUNT_OF(tempStringBuf));
		if (!ret)
		{
			ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
			taskFatalError();
		}
		// ESP_LOGI(TAG, "Exec Doc: %s", tempStringBuf);

		/* Then find job document inside execution document and store in current job*/
		jsmntok_t *jobDocToken = findToken("jobDocument", params->payload, execDocToken);
		ret = extractJsonTokenAsString(jobDocToken, (char *)params->payload, &currentJob.jobDoc[0], COUNT_OF(currentJob.jobDoc));
		if (!ret)
		{
			ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
			taskFatalError();
		}
		// ESP_LOGI(TAG, "Job Doc: %s", currentJob.jobDoc);

		/* Since a job has been found, set JOB_READY_BIT */
        xEventGroupSetBits(taskEventGroup, JOB_READY_BIT);
	}
}


void awsGetJobUpdateCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData)
{
    ESP_LOGI(TAG, "\n Job Update Callback: %.*s\t%.*s\n", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);
}


void awsProcessJob(void)
{
    bool ret = false;

    xEventGroupSetBits(taskEventGroup, JOB_PROCESSING_BIT);

    jsmn_init(&jParser);
    int r = jsmn_parse(&jParser, currentJob.jobDoc, COUNT_OF(currentJob.jobDoc), jTokens, COUNT_OF(jTokens));

    /* Check if parsed json is valid */
    if (r < 0)
    {
        ESP_LOGE(TAG, "Process Job: Failed to parse JSON: %d", r);
        taskFatalError();
    }

    if ((r < 1) || (jTokens[0].type != JSMN_OBJECT))
    {
        ESP_LOGE(TAG, "Process Job: Expected object");
        taskFatalError();
    }

    /* Then find job type */
    jsmntok_t *jobTypeToken = findToken("operation", currentJob.jobDoc, &jTokens[0]);
    ret = extractJsonTokenAsString(jobTypeToken, (char *)currentJob.jobDoc, &tempStringBuf[0], COUNT_OF(tempStringBuf));
    if (!ret)
    {
        ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
        taskFatalError();
    }
    // ESP_LOGI(TAG, "Job Type: %s", tempStringBuf);

    if (strcmp(tempStringBuf, "ota") == 0)
    {
        char appVersionBuf[32];

        if (currentJob.inProgress)
        {
            /* If we have an inProgress OTA job then check if appVersion in job doc is same as the currently running
             * firmware.
             *
             * If so, then could do other validation checks to ascertain as to whether FW is valid, but currently
             * treat this as a mark of OTA update success.
             *
             * If versions are different then OTA failure, most likely due to an app booting and failing validation
             * checks before connecting to AWS
             */
            const esp_partition_t *running = esp_ota_get_running_partition();
            esp_app_desc_t currentAppInfo;
            if (esp_ota_get_partition_description(running, &currentAppInfo) == ESP_OK)
            {
                /* For OTA we expect appVersion in job doc so search for it */
                jsmntok_t *verToken = findToken("appVersion", (char *)currentJob.jobDoc, &jTokens[0]);
                ret = extractJsonTokenAsString(verToken, (char *)currentJob.jobDoc, &appVersionBuf[0], COUNT_OF(appVersionBuf));
                if (!ret)
                {
                    ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
                    taskFatalError();
                }

                if (strncmp(appVersionBuf, currentAppInfo.version, strlen(currentAppInfo.version)) == 0)
                {
                    /* App versions match as expected so can mark job as complete */
                    ESP_LOGI(TAG, "App version matches job version, OTA job complete");
                    xEventGroupSetBits(taskEventGroup, JOB_COMPLETED_BIT);
                }
                else
                {
                    /* App versions do not match, job failure */
                    ESP_LOGW(TAG, "Current FW version does not match that of AWS OTA job, job failed");
                    ESP_LOGW(TAG, "Current FW: %s\tappVersion: %s", currentAppInfo.version, appVersionBuf);
                    snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg),
                             "Current FW version does not match version specified in OTA job");
                    xEventGroupSetBits(taskEventGroup, JOB_FAILED_BIT);
                }
            }
            else
            {
                ESP_LOGW(TAG, "Cannot check if OTA update is valid, unable to obtain current FW version");
                snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg),
                         "Cannot check if OTA update is valid, unable to obtain current FW version");
                xEventGroupSetBits(taskEventGroup, JOB_FAILED_BIT);
            }
        }
        else
        {
            /* For OTA we expect appVersion in job doc so search for it */
            jsmntok_t *verToken = findToken("appVersion", (char *)currentJob.jobDoc, &jTokens[0]);
            ret = extractJsonTokenAsString(verToken, (char *)currentJob.jobDoc, &appVersionBuf[0], COUNT_OF(appVersionBuf));
            if (!ret)
            {
                ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
                taskFatalError();
            }
            // ESP_LOGI(TAG, "appVersion: %s", appVersionBuf);

            /* For OTA we expect url contained in job doc so search for it*/
            jsmntok_t *urlToken = findToken("url", currentJob.jobDoc, &jTokens[0]);
            ret = extractJsonTokenAsString(urlToken, (char *)currentJob.jobDoc, &tempStringBuf[0], COUNT_OF(tempStringBuf));
            if (!ret)
            {
                ESP_LOGE(TAG, "JSON token extraction fail, buffer overflow or token is NULL");
                taskFatalError();
            }
            // ESP_LOGI(TAG, "url: %s", tempStringBuf);

            ma_ota_err_t otaErr = httpsOtaUpdate(&appVersionBuf[0], &tempStringBuf[0], s3_root_cert_start);
            if (otaErr != MA_OTA_SUCCESS)
            {
                ESP_LOGE(TAG, "HTTPS OTA update failed: %d", otaErr);
                xEventGroupSetBits(taskEventGroup, JOB_FAILED_BIT);

                switch (otaErr)
                {
                    case MA_OTA_DUP_FW :
                    {
                        snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg), "Tried to install FW with same version as previous");
                        break;
                    }
                    case MA_OTA_INV_FW :
                    {
                        snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg), "Installed FW marked as invalid, running previous version");
                        break;
                    }
                    case MA_OTA_HTTP_ERR :
                    {
                        snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg), "Error downloading OTA update");
                        break;
                    }
                    case MA_OTA_JOB_ERR :
                    {
                        snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg), "Mismatch between appVersion in job doc and version in binary");
                        break;
                    }
                    case MA_OTA_ERR :
                    {
                        snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg), "Generic OTA error occurred");
                        break;
                    }
                    default :
                    {
                        snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg), "OTA error not registered");
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "HTTPS OTA update successful, request restart");
                xEventGroupSetBits(taskEventGroup, RESTART_REQUESTED_BIT);
            }
        }
    }
    else if (strcmp(tempStringBuf, "systemLog") == 0)
    {

    }
    else if (strcmp(tempStringBuf, "restart") == 0)
    {
        ESP_LOGI(TAG, "Requesting restart");
        xEventGroupSetBits(taskEventGroup, RESTART_REQUESTED_BIT);
    }
    else
    {
        ESP_LOGE(TAG, "Job operation type not found: %s", tempStringBuf);
        snprintf(currentJob.jobFailMsg, COUNT_OF(currentJob.jobFailMsg), "Job operation not found");
        xEventGroupSetBits(taskEventGroup, JOB_FAILED_BIT);
    }
}


static void awsUpdateAcceptedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData)
{
    ESP_LOGI(TAG, "JOB_UPDATE_TOPIC / accepted callback");
    ESP_LOGI(TAG, "topic: %.*s", topicNameLen, topicName);
    ESP_LOGI(TAG, "payload: %.*s", (int) params->payloadLen, (char *)params->payload);
}

static void awsUpdateRejectedCallbackHandler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData)
{
    ESP_LOGI(TAG, "JOB_UPDATE_TOPIC / rejected callback");
    ESP_LOGI(TAG, "topic: %.*s", topicNameLen, topicName);
    ESP_LOGI(TAG, "payload: %.*s", (int) params->payloadLen, (char *)params->payload);

    /* Do error handling here for when the update was rejected */
}


static void connectToAWS(void)
{
	IoT_Error_t rc = FAILURE;

	mqttInitParams = iotClientInitParamsDefault;
	connectParams = iotClientConnectParamsDefault;

    ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;

    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
    mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;

    mqttInitParams.mqttCommandTimeout_ms = 30000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = NULL;

    rc = aws_iot_mqtt_init(&client, &mqttInitParams);
    if (SUCCESS != rc)
    {
        ESP_LOGE(TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        taskFatalError();
    }

    /* Reasonably long keep alive interval to deal with MQTT dead time during OTA update */
    connectParams.keepAliveIntervalInSec = 30;
    connectParams.isCleanSession = true;
    connectParams.MQTTVersion = MQTT_3_1_1;
    /* Client ID is set in the menuconfig of the example */
    connectParams.pClientID = CONFIG_AWS_EXAMPLE_CLIENT_ID;
    connectParams.clientIDLen = (uint16_t) strlen(CONFIG_AWS_EXAMPLE_CLIENT_ID);
    connectParams.isWillMsgPresent = false;

    ESP_LOGI(TAG, "Connecting to AWS...");
	do
	{
		rc = aws_iot_mqtt_connect(&client, &connectParams);
		if (SUCCESS != rc)
		{
			ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
			vTaskDelay(1000 / portTICK_PERIOD_MS);
		}
	} while(SUCCESS != rc);

	/*
	 * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
	 *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
	 *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
	 */
	rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
	if (SUCCESS != rc)
	{
		ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
		taskFatalError();
	}

    paramsQOS0.qos = QOS0;
    paramsQOS0.payload = (void *) testPayload;
    paramsQOS0.isRetained = 0;

    /* Create and subscribe to job topics */
    rc = aws_iot_jobs_subscribe_to_job_messages(&client, QOS0, THING_NAME, NULL, JOB_GET_PENDING_TOPIC, JOB_REJECTED_REPLY_TYPE,
                                                awsGetRejectedCallbackHandler, NULL, getRejectedSubBuf, COUNT_OF(getRejectedSubBuf));
    if (SUCCESS != rc)
    {
        ESP_LOGE(TAG, "Unable to subscribe JOB_GET_REJECTED: %d", rc);
        taskFatalError();
    }

    rc = aws_iot_jobs_subscribe_to_job_messages(&client, QOS0, THING_NAME, NULL, JOB_GET_PENDING_TOPIC, JOB_ACCEPTED_REPLY_TYPE,
                                                awsGetAcceptedCallbackHandler, NULL, getAcceptedSubBuf, COUNT_OF(getAcceptedSubBuf));
    if (SUCCESS != rc)
    {
        ESP_LOGE(TAG, "Unable to subscribe to JOB_GET_ACCEPTED: %d", rc);
        taskFatalError();
    }

    int num = snprintf(getJobAcceptedSubBuf, COUNT_OF(getJobAcceptedSubBuf), "$aws/things/%s/jobs/+/get/accepted", THING_NAME);
    rc = aws_iot_mqtt_subscribe(&client, getJobAcceptedSubBuf, num, QOS0,
        						awsJobGetAcceptedCallbackHandler, NULL);
    if (SUCCESS != rc)
    {
        ESP_LOGE(TAG, "Unable to subscribe to JOBS_DESCRIBE_TOPIC: %d", rc);
        taskFatalError();
    }

    rc = aws_iot_jobs_subscribe_to_job_messages(&client, QOS0, THING_NAME, JOB_ID_WILDCARD, JOB_UPDATE_TOPIC, JOB_ACCEPTED_REPLY_TYPE,
                                                awsUpdateAcceptedCallbackHandler, NULL, getJobUpdateAcceptedSubBuf, COUNT_OF(getJobUpdateAcceptedSubBuf));
    if (SUCCESS != rc)
    {
        ESP_LOGE(TAG, "Unable to subscribe to JOB_UPDATE_ACCEPTED: %d", rc);
        taskFatalError();
    }

    rc = aws_iot_jobs_subscribe_to_job_messages(&client, QOS0, THING_NAME, JOB_ID_WILDCARD, JOB_UPDATE_TOPIC, JOB_REJECTED_REPLY_TYPE,
                                                awsUpdateRejectedCallbackHandler, NULL, getJobUpdateRejectedSubBuf, COUNT_OF(getJobUpdateRejectedSubBuf));
    if (SUCCESS != rc)
	{
		ESP_LOGE(TAG, "Unable to subscribe to JOB_UPDATE_REJECTED: %d", rc);
		taskFatalError();
	}

    /* Loop for AWS processing stage */
    while ((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc))
    {
    	ESP_LOGI(TAG, "In AWS processing loop------------------------------------------------------");

        /* Yield pauses the current thread, allowing MQTT send/receive to occur.
         *      - Essentially use this as a delay, all time spent not working in this
         *        loop should be spent in yield
         */
        rc = aws_iot_mqtt_yield(&client, 5000);

        if(NETWORK_ATTEMPTING_RECONNECT == rc)
        {
            // If the client is attempting to reconnect then skip the rest of the loop.
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        /* If a restart is requested, unsub and then restart */
        if ((xEventGroupGetBits(taskEventGroup) & RESTART_REQUESTED_BIT) != 0)
        {
            ESP_LOGI(TAG, "RESTART_REQUESTED_BIT processing");
            unsubAndDisconnnectAWS();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        }

        /* Only query for more jobs if not currently processing a job */
        if ((xEventGroupGetBits(taskEventGroup) & JOB_PROCESSING_BIT) == 0)
        {
            ESP_LOGI(TAG, "AWS_QUERY processing");
            rc = aws_iot_jobs_send_query(&client, QOS0, THING_NAME, NULL, NULL, tempJobBuf, COUNT_OF(tempJobBuf),
                                         NULL, 0, JOB_GET_PENDING_TOPIC);
        }

        /* We can process a job locally if a job is ready from AWS */
        if ((xEventGroupGetBits(taskEventGroup) & JOB_READY_BIT) != 0)
        {
            ESP_LOGI(TAG, "JOB_READY_BIT processing");

            if (!currentJob.inProgress)
            {
                AwsIotJobExecutionUpdateRequest updateRequest;
                updateRequest.clientToken = NULL;
                updateRequest.includeJobDocument = false;
                updateRequest.includeJobExecutionState = false;
                updateRequest.executionNumber = 0;
                updateRequest.expectedVersion = 0;
                updateRequest.status = JOB_EXECUTION_IN_PROGRESS;
                updateRequest.statusDetails = NULL;
                rc = aws_iot_jobs_send_update(&client, QOS0, THING_NAME, currentJob.jobId, &updateRequest, tempJobBuf, COUNT_OF(tempJobBuf), tempStringBuf, COUNT_OF(tempStringBuf));
                if (rc != SUCCESS)
                {
                    ESP_LOGE(TAG, "AWS Job Update notification failed: %d", rc);
                    taskFatalError();
                }
                // ESP_LOGW(TAG, "%s", tempStringBuf);
            }

            /* Try and execute the job */
            awsProcessJob();
        }

        /* We can notify AWS as job completed once verified complete */
        if ((xEventGroupGetBits(taskEventGroup) & JOB_COMPLETED_BIT) != 0)
        {
            ESP_LOGI(TAG, "JOB_COMPLETED_BIT processing");

            /* Notify AWS that current job has completed successfully */
            AwsIotJobExecutionUpdateRequest updateRequest;
            updateRequest.clientToken = NULL;
            updateRequest.includeJobDocument = false;
            updateRequest.includeJobExecutionState = false;
            updateRequest.executionNumber = 0;
            updateRequest.expectedVersion = 0;
            updateRequest.status = JOB_EXECUTION_SUCCEEDED;
            updateRequest.statusDetails = NULL;
            rc = aws_iot_jobs_send_update(&client, QOS0, THING_NAME, currentJob.jobId, &updateRequest, tempJobBuf, COUNT_OF(tempJobBuf), tempStringBuf, COUNT_OF(tempStringBuf));
            if (rc != SUCCESS)
            {
                ESP_LOGE(TAG, "AWS Job Update notification failed: %d", rc);
                taskFatalError();
            }

            /* Reset all in job in completion flags to continue processing other jobs */
            xEventGroupClearBits(taskEventGroup, JOB_PROCESSING_BIT | JOB_COMPLETED_BIT | JOB_READY_BIT);
        }

        /* Notify AWS if current job failed */
        if ((xEventGroupGetBits(taskEventGroup) & JOB_FAILED_BIT) != 0)
        {
            ESP_LOGW(TAG, "JOB_FAILED_BIT processing");

            /* Must format jobFailMsg into key-value pair for JSON doc to AWS */
            char tempRetBuf[128];
            bool ret = formatJobErrorStatusDetails(currentJob.jobFailMsg, tempRetBuf, COUNT_OF(tempRetBuf));
            ESP_LOGW(TAG, "Error message: %s", tempRetBuf);
            if (!ret)
            {
                taskFatalError();
            }

            AwsIotJobExecutionUpdateRequest updateRequest;
            updateRequest.clientToken = NULL;
            updateRequest.includeJobDocument = false;
            updateRequest.includeJobExecutionState = false;
            updateRequest.executionNumber = 0;
            updateRequest.expectedVersion = 0;
            updateRequest.status = JOB_EXECUTION_FAILED;
            updateRequest.statusDetails = tempRetBuf;
            rc = aws_iot_jobs_send_update(&client, QOS0, THING_NAME, currentJob.jobId, &updateRequest, tempJobBuf, COUNT_OF(tempJobBuf), tempStringBuf, COUNT_OF(tempStringBuf));
            if (rc != SUCCESS)
            {
                ESP_LOGE(TAG, "AWS Job Update notification failed: %d", rc);
                taskFatalError();
            }

            /* Reset all in job in completion flags to continue processing other jobs */
            xEventGroupClearBits(taskEventGroup, JOB_PROCESSING_BIT | JOB_COMPLETED_BIT | JOB_READY_BIT | JOB_FAILED_BIT);
        }

        ESP_LOGI(TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
    }

    /* If the while loop exits unintentionally then try and unsub and disconnect before exiting */
    unsubAndDisconnnectAWS();
}


static void unsubAndDisconnnectAWS(void)
{
    IoT_Error_t rc = FAILURE;

    rc = aws_iot_mqtt_yield(&client, 100);

    /* Unsubscribe from all AWS services */
    rc = aws_iot_jobs_unsubscribe_from_job_messages(&client, getRejectedSubBuf);
    rc = aws_iot_jobs_unsubscribe_from_job_messages(&client, getAcceptedSubBuf);
    rc = aws_iot_jobs_unsubscribe_from_job_messages(&client, getJobAcceptedSubBuf);
    rc = aws_iot_jobs_unsubscribe_from_job_messages(&client, getJobUpdateAcceptedSubBuf);
    rc = aws_iot_jobs_unsubscribe_from_job_messages(&client, getJobUpdateRejectedSubBuf);

    rc = aws_iot_mqtt_yield(&client, 500);

    if (rc != SUCCESS)
    {
        ESP_LOGE(TAG, "Unable to unsubscribe from AWS Jobs service: %d", rc);
        taskFatalError();
    }
    else
    {
        ESP_LOGI(TAG, "Unsubscribed from AWS jobs services");
    }

    /* Disconnect from AWS */
    rc = aws_iot_mqtt_disconnect(&client);
    if (rc != SUCCESS)
    {
        ESP_LOGE(TAG, "Unable to disconnect from AWS: %d", rc);
        taskFatalError();
    }
    else
    {
        ESP_LOGI(TAG, "Disconnected from AWS");
    }
}


void job_agent_task(void *param)
{
    taskEventGroup = xEventGroupCreate();

	bootValidityCheck();
	ESP_ERROR_CHECK(nvsBootCheck());
	otaBootCheck();

	initialiseWifi();

	while (1)
	{
		/* Check if WiFi provisioned */
		if (wifiProvisionedCheck() == 1)
		{
			ESP_LOGI(TAG, "WiFi provisioned");
		}
		else
		{
			ESP_LOGW(TAG, "WiFi not provisioned, waiting on credentials");
			vTaskDelay(2000 / portTICK_PERIOD_MS);
			continue;
		}

		/* Check if WiFi connected */
		EventBits_t ret_bit = xEventGroupWaitBits(taskEventGroup, WIFI_CONNECTED_BIT, false, true, 50 / portTICK_PERIOD_MS);
		if ((ret_bit & WIFI_CONNECTED_BIT) != 0)
		{
			ESP_LOGI(TAG, "WiFi connected");
		}
		else
		{
			ESP_LOGW(TAG, "WiFi not connected");
			vTaskDelay(2000 / portTICK_PERIOD_MS);
			continue;
		}

		/* Connect to AWS and process jobs*/
		connectToAWS();

		ESP_LOGE(TAG, "An error occurred in the main loop.");
		taskFatalError();
	}
}

/**
 * @file ma_jobParser.h
 * @brief Contains all details of the job parser engine
 * @author Jack Cripps // jackc@monitoraudio.com
 * @date 21/10/2019
 */
#include "jsmn.h"
#include "ma_jobParser.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#include "esp_log.h"



static const char *TAG = "ma_JobParser";

jsmntok_t* findJsonToken(int numTokens, const char *key, const char *jsonString, jsmntok_t *token)
{
	jsmntok_t *result = token;
	int16_t i;

	if (token->type != JSMN_OBJECT)
	{
		ESP_LOGW(TAG, "Token was not an object.");
		return NULL;
	}

	if (token->size == 0)
	{
		return NULL;
	}

	for (i = 0; i < numTokens; ++i)
	{
		if (jsoneq(jsonString, result, key) == 0)
		{
			return result + 1;
		}
	}

	return NULL;
}

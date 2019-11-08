/**
 * @file ma_jobParser.h
 * @brief Contains all details of the job parser engine
 * @author Jack Cripps // jackc@monitoraudio.com
 * @date 21/10/2019
 */

#ifndef __JOB_PARSER_H__
#define __JOB_PARSER_H__

#include "jsmn.h"

jsmntok_t* findJsonToken(int numTokens, const char *key, const char *jsonString, jsmntok_t *token);

#endif /*__JOB_PARSER_H__*/

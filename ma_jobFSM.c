/* Job Parse FSM Implementation
 * -------------------------------------------------------------------------- */

int entryState(void);
int queryState(void);
int describeState(void);
int executeState(void);
int finishState(void);

/* Must keep array and enum synchronised, apart from fsm error state */
int (* state[])(void) = {entryState, queryState, describeState, executeState, finishState};
enum state_code {entry, query, describe, execute, finish, fsmError};

/* State table defining flow */
enum ret_code {success, failure, repeat};
struct transition {
	enum state_code srcState;
	enum ret_code retCode;
	enum state_code destState;
};

struct transition stateTransitions[] = {
	{entry, success, query},
	{entry, failure, finish},
	{query, success, describe},
	{query, failure, finish},
	{describe, success, execute},
	{describe, failure, finish},
	{execute, success, finish},
	{execute, failure, finish},
	{execute, repeat, describe},
};

enum state_code lookupTransition(enum state_code state, enum ret_code ret)
{
	enum state_code nextState = fsmError;

	for (uint8_t i = 0; i < COUNT_OF(stateTransitions); ++i)
	{
		if ((stateTransitions[i].srcState == state) && (stateTransitions[i].retCode == ret))
		{
			nextState = stateTransitions[i].destState;
			break;
		}
	}

	return nextState;
}

/* Start the state machine at entry state */
enum state_code currentState = entry;
enum ret_code returnCode;
int (* stateFunc)(void);

int entryState(void)
{
	enum ret_code ret = fsmError;

	/* Subscribe to all job messages */
	IoT_Error_t rc = aws_iot_jobs_subscribe_to_all_job_messages(&client, QOS0, THING_NAME, iotSubscribeCallbackHandler,
													NULL, allJobSubBuf, COUNT_OF(allJobSubBuf));
	if (rc != SUCCESS)
	{
		ESP_LOGE(TAG, "Error subscribing to all jobs: %d", rc);
		ret = failure;
	}
	else
	{
		ret = success;
	}

	return ret;
}


int queryState(void)
{
	enum ret_code ret = fsmError;

	IoT_Error_t rc = aws_iot_jobs_send_query(&client, QOS0, THING_NAME, NULL, NULL, tempJobBuf, COUNT_OF(tempJobBuf),
	        					 	 	 	 NULL, NULL, JOB_GET_PENDING_TOPIC);
	if (rc != SUCCESS)
	{
		ESP_LOGE(TAG, "Error subscribing to all jobs: %d", rc);
		ret = failure;
	}
	else
	{
		ret = success;
	}

	return ret;
}


int describeState(void)
{
	enum ret_code ret = fsmError;

	return ret;
}


int executeState(void)
{
	enum ret_code ret = fsmError;

	return ret;
}


int finishState(void)
{
	enum ret_code ret = fsmError;

	IoT_Error_t rc = aws_iot_jobs_unsubscribe_from_job_messages(&client, allJobSubBuf);
	if (rc != SUCCESS)
	{
		ESP_LOGE(TAG, "Error unsubscribing to all jobs: %d", rc);
		ret = failure;
	}
	else
	{
		ret = success;
	}

	return ret;
}

stateFunc = state[currentState];
returnCode = stateFunc();
if (currentState == finish)
{
	break;
}
else
{
	currentState = lookupTransition(currentState, returnCode);
}

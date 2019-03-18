/*
 * Amazon FreeRTOS Shadow Demo V1.4.6
 * Copyright (C) 2017 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @file `
 * @brief A simple shadow example.
 *
 * The simple Shadow lightbulb example to illustrate how client application and
 * things communicate with the Shadow service.
 */

/* Standard includes. */
#include <stdio.h>
#include <string.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

#include <driver/gpio.h>

/* MQTT include. */
#include "aws_mqtt_agent.h"

/* Demo configurations. */
#include "aws_demo_config.h"

/* Required to get the broker address and port. */
#include "aws_clientcredential.h"

/* Required for shadow APIs. */
#include "aws_shadow.h"

/* Required for shadow demo. */
#include "aws_shadow_led.h"
#include "jsmn.h"

/* Task names. */
#define shadowDemoCHAR_TASK_NAME           "Shd-IOT-%d"
#define shadowDemoUPDATE_TASK_NAME         "ShDemoUpdt"


static BaseType_t prvDoStateUpdate( const char * pcStateJsonString,
                                    uint32_t ulStateJsonStringLength );
static uint32_t prvGenerateStateJSON( const char * pcStateJsonString );


/* Maximum amount of time a Shadow function call may block. */
#define shadowDemoTIMEOUT                    pdMS_TO_TICKS( 30000UL )

/* Max size for the name of tasks generated for Shadow. */
#define shadowDemoCHAR_TASK_NAME_MAX_SIZE    15

/* Name of the thing. */
#define shadowDemoTHING_NAME                 clientcredentialIOT_THING_NAME

/* Maximum size of update JSON documents. */
#define shadowDemoBUFFER_LENGTH              512

/* Stack size for task that handles shadow delta and updates. */
#define shadowDemoUPDATE_TASK_STACK_SIZE     ( ( uint16_t ) configMINIMAL_STACK_SIZE * ( uint16_t ) 5 )

/* Maximum number of jsmn tokens. */
#define shadowDemoMAX_TOKENS                 40

/* Queue configuration parameters. */
#define shadowDemoSEND_QUEUE_WAIT_TICKS      3000
#define shadowDemoRECV_QUEUE_WAIT_TICKS      500
#define shadowDemoUPDATE_QUEUE_LENGTH        democonfigSHADOW_DEMO_NUM_TASKS * 2

/* The maximum amount of time tasks will wait for their updates to process.
 * Tasks should not continue executing until their updates have processed.*/
#define shadowDemoNOTIFY_WAIT_MS             pdMS_TO_TICKS( ( 100000UL ) )

/* An element of the Shadow update queue. */
typedef struct
{
    TaskHandle_t xTaskToNotify;
    uint32_t ulDataLength;
    char pcUpdateBuffer[ shadowDemoBUFFER_LENGTH ];
} ShadowQueueData_t;

/* The parameters of the Shadow tasks. */
typedef struct
{
    TaskHandle_t xTaskHandle;
    char cTaskName[ shadowDemoCHAR_TASK_NAME_MAX_SIZE ];
} ShadowTaskParam_t;

/* Shadow demo tasks. */
static void prvShadowInitTask( void * pvParameters );
static void prvUpdateQueueTask( void * pvParameters );

/* Creates Shadow client and connects to MQTT broker. */
static ShadowReturnCode_t prvShadowClientCreateConnect( void );

/* Called when there's a difference between "reported" and "desired" in Shadow document. */
static BaseType_t prvDeltaCallback( void * pvUserData,
                                    const char * const pcThingName,
                                    const char * const pcDeltaDocument,
                                    uint32_t ulDocumentLength,
                                    MQTTBufferHandle_t xBuffer );

/* JSON functions. */
static uint32_t prvGenerateReportedJSON( ShadowQueueData_t * const pxShadowQueueData,
                                         const char * const pcReportedData );
static void prvExtractTokenString( const char * const pcJson,     /*lint !e971 can use char without signed/unsigned. */
                            const jsmntok_t * const pxTok,
                            const char * const pcString ); /*lint !e971 can use char without signed/unsigned. */

/* The update queue's handle, data structure, and memory. */
static QueueHandle_t xUpdateQueue = NULL;
static StaticQueue_t xStaticQueue;
static uint8_t ucQueueStorageArea[ shadowDemoUPDATE_QUEUE_LENGTH * sizeof( ShadowQueueData_t ) ];

/* The handle of the Shadow client shared across all tasks. */
static ShadowClientHandle_t xClientHandle;


/*-----------------------------------------------------------*/

/* JSON formats used in the Shadow tasks. Note the inclusion of the "clientToken"
 * key, which is REQUIRED by the Shadow API. The "clientToken" may be anything, as
 * long as it's unique. This demo uses "token-" suffixed with the RTOS tick count
 * at the time the JSON document is generated. */
#define shadowDemoREPORT_JSON       \
    "{"                             \
    "\"state\":{"                   \
    "\"reported\":"                 \
    "%s"                          \
    "},"                            \
    "\"clientToken\": \"token-%d\"" \
    "}"


static uint32_t prvGenerateReportedJSON( ShadowQueueData_t * const pxShadowQueueData,
                                         const char * const pcReportedData )
{
    return ( uint32_t ) snprintf( ( char * ) ( pxShadowQueueData->pcUpdateBuffer ),
                                  shadowDemoBUFFER_LENGTH,
                                  shadowDemoREPORT_JSON,
                                  pcReportedData,
                                  ( int ) xTaskGetTickCount() );
}

/*-----------------------------------------------------------*/

/*
 * given a JSMN "token" (i.e. could be a node name, another node, or...), if it's
 *  corresponds to a piece of the JSON document itself, pull it out
 */
static void prvExtractTokenString( const char * pcJsonDocumentString,    /*lint !e971 can use char without signed/unsigned. */
                                const jsmntok_t * pxTok,
                                const char * pcTokenString ) /*lint !e971 can use char without signed/unsigned. */
{
    uint32_t ulStringSize = ( uint32_t ) pxTok->end - ( uint32_t ) pxTok->start + 1;
    snprintf( ( char * ) pcTokenString, ulStringSize, "%.*s", ulStringSize, pcJsonDocumentString + pxTok->start );
}

/*-----------------------------------------------------------*/

/*
 * given a JSON string, pull out the string value of the given
 *  node, if it's in the top level
 *
 * for example, given this JSON (whitespace added for readability):
    {
       "foo": "bar",
       "values": {
          "a": 1,
          "b": 2
       }
    }
 *
 * then calling this function with nodeName "foo" will extract "bar", calling
 *  this with "values" will extract '{"a":1,"b":2}' and "x" will extract ""
 *
 * note that this does not distinguish between nodes where the value
 *  is actually "" and where the node doesn't exist
 */
static uint32_t prvExtractTopLevelJsonNode( const char *pcNodeValue,
                                            const uint16_t pcNodeValueLength,
                                            const char * pcJsonString,
                                            const char * pcDesiredNodeName )
{
    uint32_t ulNodeValueLength;
    int32_t lNbTokens;
    uint16_t usTokenIndex;
    jsmn_parser xJSMNParser;
    jsmntok_t pxJSMNTokens[ shadowDemoMAX_TOKENS ];
    char pcTokenString[shadowDemoBUFFER_LENGTH];

    jsmn_init( &xJSMNParser );

    lNbTokens = ( int32_t ) jsmn_parse( &xJSMNParser,
                                        pcJsonString,
                                        ( size_t ) strlen( pcJsonString ),
                                        pxJSMNTokens,
                                        ( unsigned int ) shadowDemoMAX_TOKENS );

    ulNodeValueLength = 0;

    for( usTokenIndex = 0; usTokenIndex < ( uint16_t ) lNbTokens; usTokenIndex++ )
    {
        jsmntok_t jToken = pxJSMNTokens[ usTokenIndex ];
        configPRINTF( ( "token %d is type %d\r\n", usTokenIndex, jToken.type ));

        if( jToken.type == JSMN_STRING )
        {
            prvExtractTokenString( pcJsonString, &jToken, pcTokenString);
            configPRINTF( ( "token %d is '%s'\r\n", usTokenIndex, pcTokenString ));

            if( strcmp( pcTokenString, pcDesiredNodeName ) == 0 )
            {
                jsmntok_t nextToken = pxJSMNTokens[ usTokenIndex + 1 ];

                prvExtractTokenString( pcJsonString, &nextToken, pcNodeValue );
                ulNodeValueLength = strlen(pcNodeValue);

                configPRINTF( ( "extracted node contents: '%s'\r\n", pcNodeValue ) );

                break;
            }
        }
    }

    if( ulNodeValueLength == 0 )
    {
        configPRINTF( ( "did not find '%s' in JSON: %s\r\n", pcDesiredNodeName, pcJsonString) );
    }

    return ulNodeValueLength;
}

/*-----------------------------------------------------------*/

/*
 * this processes the '/update/delta' message... which only comes if someone ELSE has changed the state
 *
 * the message will look like this, and only the contents of 'state' should differ between
 *  different devices
 *
       {
            "version":9009,
            "timestamp":1548978205,
            "state":{
                "foo":"bar",
                "goo":14
            },
            "metadata":{
                ...
            },
            "clientToken":"134"
       }
 */
static BaseType_t prvDeltaCallback( void * pvUserData,
                                    const char * pcThingName,
                                    const char * pcDeltaDocument,
                                    uint32_t ulDocumentLength,
                                    MQTTBufferHandle_t xBuffer )
{
    char pcStateNodeString[shadowDemoBUFFER_LENGTH];
    uint32_t ulStateNodeStringLength;
    ShadowQueueData_t xShadowQueueData;

    /* Silence compiler warnings about unused variables. */
    ( void ) pvUserData;
    ( void ) xBuffer;
    ( void ) pcThingName;

    memset( &xShadowQueueData, 0x00, sizeof( ShadowQueueData_t ) );

    configPRINTF( ( "processing message from /update/delta: %s\r\n", pcDeltaDocument ) );

    ulStateNodeStringLength = prvExtractTopLevelJsonNode( pcStateNodeString,
                                                          shadowDemoBUFFER_LENGTH,
                                                          pcDeltaDocument,
                                                          "state" );

    if( ulStateNodeStringLength > 0 )
    {
        configPRINTF( ( "desired state: '%s'\r\n", pcStateNodeString ) );

        if( prvDoStateUpdate( pcStateNodeString, ulStateNodeStringLength ) == pdTRUE )
        {
            prvGenerateStateJSON( pcStateNodeString );

            /* Generate a new JSON document with new reported state. */
            xShadowQueueData.ulDataLength = prvGenerateReportedJSON( &xShadowQueueData, pcStateNodeString );

            configPRINTF( ( "responding with: %s\r\n", xShadowQueueData.pcUpdateBuffer ) );

            /* Add new reported state to update queue. */
            if( xQueueSendToBack( xUpdateQueue, &xShadowQueueData, shadowDemoSEND_QUEUE_WAIT_TICKS ) == pdTRUE )
            {
                configPRINTF( ( "Successfully added new reported state to internal queue.\r\n" ) );
            }
            else
            {
                configPRINTF( ( "Update queue full, deferring reported state update.\r\n" ) );
            }
        }
        else
        {
            // what??
        }
    }

    return pdFALSE;
}



/*-----------------------------------------------------------*/

/*
 * this actually sends the JSON "reported" message to the MQTT /update topic
 */
static void prvUpdateQueueTask( void * pvParameters )
{
    ShadowReturnCode_t xReturn;
    ShadowOperationParams_t xUpdateParams;
    ShadowQueueData_t xShadowQueueData;

    ( void ) pvParameters;

    xUpdateParams.pcThingName = shadowDemoTHING_NAME;
    xUpdateParams.xQoS = eMQTTQoS0;
    xUpdateParams.pcData = xShadowQueueData.pcUpdateBuffer;
    /* Keep subscriptions across multiple calls to SHADOW_Update. */
    xUpdateParams.ucKeepSubscriptions = pdTRUE;

    for( ; ; )
    {
        if( xQueueReceive( xUpdateQueue, &xShadowQueueData, shadowDemoRECV_QUEUE_WAIT_TICKS ) == pdTRUE )
        {
            configPRINTF( ( "Sending update to Shadow Device service: (%s)\r\n", xShadowQueueData.pcUpdateBuffer ) );
            xUpdateParams.ulDataLength = xShadowQueueData.ulDataLength;

            xReturn = SHADOW_Update( xClientHandle, &xUpdateParams, shadowDemoTIMEOUT );

            if( xReturn == eShadowSuccess )
            {
                configPRINTF( ( "Shadow device update successfully published.\r\n" ) );
            }
            else
            {
                configPRINTF( ( "Shadow device update failed, returned %d.\r\n", xReturn ) );
            }

            /* Notify tasks that their update was completed. */
            if( xShadowQueueData.xTaskToNotify != NULL )
            {
                xTaskNotifyGive( xShadowQueueData.xTaskToNotify );
            }
        }
    }
}

/*-----------------------------------------------------------*/

static ShadowReturnCode_t prvShadowClientCreateConnect( void )
{
    MQTTAgentConnectParams_t xConnectParams;
    ShadowCreateParams_t xCreateParams;
    ShadowReturnCode_t xReturn;

    configPRINTF( ( "Calling SHADOW_ClientCreate...\r\n") );

    xCreateParams.xMQTTClientType = eDedicatedMQTTClient;
    xReturn = SHADOW_ClientCreate( &xClientHandle, &xCreateParams );

    if( xReturn == eShadowSuccess )
    {
        memset( &xConnectParams, 0x00, sizeof( xConnectParams ) );
        xConnectParams.pcURL = clientcredentialMQTT_BROKER_ENDPOINT;
        xConnectParams.usPort = clientcredentialMQTT_BROKER_PORT;

        xConnectParams.xFlags = democonfigMQTT_AGENT_CONNECT_FLAGS;
        xConnectParams.pcCertificate = NULL;
        xConnectParams.ulCertificateSize = 0;
        xConnectParams.pxCallback = NULL;
        xConnectParams.pvUserData = &xClientHandle;

        xConnectParams.pucClientId = ( const uint8_t * ) ( clientcredentialIOT_THING_NAME );
        xConnectParams.usClientIdLength = ( uint16_t ) strlen( clientcredentialIOT_THING_NAME );


        int attempts = 0;
        do
        {
            configPRINTF( ( "Calling SHADOW_ClientConnect...\r\n") );
            xReturn = SHADOW_ClientConnect( xClientHandle,
                                            &xConnectParams,
                                            shadowDemoTIMEOUT );

            if( xReturn != eShadowSuccess )
            {
                configPRINTF( ( "Shadow_ClientConnect unsuccessful, returned %d.\r\n", xReturn ) );
                attempts ++;
            }
            else
            {
                configPRINTF( ( "Shadow_ClientConnect successful\r\n" ) );
            }
        }
        while( (attempts < 3) && (xReturn != eShadowSuccess));
    }
    else
    {
        configPRINTF( ( "Shadow_ClientCreate unsuccessful, returned %d.\r\n", xReturn ) );
    }

    return xReturn;
}

/*-----------------------------------------------------------*/

/*
 * connect to AWS and set up a 'task' (a separate thread) for processing
 *  deltas sent from the Shadow Service
 */
static void prvShadowInitTask( void * pvParameters )
{
    ShadowReturnCode_t xReturn;
    ShadowOperationParams_t xOperationParams;
    ShadowCallbackParams_t xCallbackParams;

    ( void ) pvParameters;

    /* Initialize the internal update queue and Shadow client; set all pending updates to false. */
    xUpdateQueue = xQueueCreateStatic( shadowDemoUPDATE_QUEUE_LENGTH,
                                       sizeof( ShadowQueueData_t ),
                                       ucQueueStorageArea,
                                       &xStaticQueue );

    xReturn = prvShadowClientCreateConnect();

    if( xReturn == eShadowSuccess )
    {
        xOperationParams.pcThingName = shadowDemoTHING_NAME;
        xOperationParams.xQoS = eMQTTQoS0;
        xOperationParams.pcData = NULL;
        /* Don't keep subscriptions, since SHADOW_Delete is only called here once. */
        xOperationParams.ucKeepSubscriptions = pdFALSE;

        /* Delete any previous shadow. WHY?? */
        configPRINTF( ( "Deleting existing shadow client...\r\n") );
        xReturn = SHADOW_Delete( xClientHandle,
                                 &xOperationParams,
                                 shadowDemoTIMEOUT );

        /* Attempting to delete a non-existant shadow returns eShadowRejectedNotFound.
         * Either eShadowSuccess or eShadowRejectedNotFound signify that there's no
         * existing Thing Shadow, so both values are ok. */
        if( ( xReturn == eShadowSuccess ) || ( xReturn == eShadowRejectedNotFound ) )
        {
            if( xReturn == eShadowSuccess )
            {
                configPRINTF( ( "Existing shadow client successfully deleted\r\n" ) );
            }
            else
            {
                configPRINTF( ( "No existing shadow client found\r\n" ) );
            }

            /* Register callbacks. This demo doesn't use updated or deleted callbacks, so
             * those members are set to NULL. The callbacks are registered after deleting
             * the Shadow so that any previous Shadow doesn't unintentionally trigger the
             * delta callback.*/
            xCallbackParams.pcThingName = shadowDemoTHING_NAME;
            xCallbackParams.xShadowUpdatedCallback = NULL;
            xCallbackParams.xShadowDeletedCallback = NULL;
            xCallbackParams.xShadowDeltaCallback = prvDeltaCallback;

            xReturn = SHADOW_RegisterCallbacks( xClientHandle,
                                                &xCallbackParams,
                                                shadowDemoTIMEOUT );

            if( xReturn != eShadowSuccess )
            {
                configPRINTF( ( "Failed to register callbacks.\r\n" ) );
            }
        }
        else
        {
            configPRINTF( ( "Failed to delete existing Shadow client: %d\r\n", xReturn ) );
        }
    }
    else
    {
        configPRINTF( ( "Failed to connect to Shadow client: %d\r\n", xReturn ) );
    }

    if( xReturn == eShadowSuccess )
    {
        configPRINTF( ( "Shadow client initialized.\r\n" ) );

        /* Create the update task which will process the internal update queue. */
        ( void ) xTaskCreate( prvUpdateQueueTask,
                              shadowDemoUPDATE_TASK_NAME,
                              shadowDemoUPDATE_TASK_STACK_SIZE,
                              NULL,
                              democonfigSHADOW_DEMO_TASK_PRIORITY,
                              NULL );
    }

    vTaskDelete( NULL );
}

/* Create the shadow demo main task which will act as a client application to
 * request periodic change in state (color) of light bulb.  */
void vStartLedDemoTasks( void )
{
    configPRINTF( ( "*************************************************\r\n" ) );
    configPRINTF( ( "*************************************************\r\n" ) );
    configPRINTF( ( "*** Rodney's LED Demo ***************************\r\n" ) );
    configPRINTF( ( "*************************************************\r\n" ) );
    configPRINTF( ( "*************************************************\r\n" ) );

    /* QUESTION: why does this need to be a task? */
    ( void ) xTaskCreate( prvShadowInitTask,
                          "MainDemoTask",
                          shadowDemoUPDATE_TASK_STACK_SIZE,
                          NULL,
                          tskIDLE_PRIORITY,
                          NULL );
}

/*-----------------------------------------------------------*/
/*-----------------------------------------------------------*/
/*-----------------------------------------------------------*/

/* details of the particular state(s) we're tracking */

#define shadowDemoSTATE_FIELD_NAME_LED_BLUE    "blue"
#define shadowDemoSTATE_FIELD_NAME_LED_GREEN   "green"
#define shadowDemoSTATE_FIELD_NAME_LED_RED     "red"
#define shadowDemoSTATE_FIELD_NAME_LED_YELLOW  "yellow"

const char pcCurrentBlueLedState[8];
const char pcCurrentGreenLedState[8];
const char pcCurrentRedLedState[8];
const char pcCurrentYellowLedState[8];


/*-----------------------------------------------------------*/


static uint32_t prvGenerateStateJSON( const char * pcStateJsonString )
{
    return ( uint32_t ) sprintf( ( char * )pcStateJsonString,
                                 "{ \"%s\":\"%s\", \"%s\":\"%s\", \"%s\":\"%s\", \"%s\":\"%s\" }",
                                 shadowDemoSTATE_FIELD_NAME_LED_BLUE,
                                 pcCurrentBlueLedState,
                                 shadowDemoSTATE_FIELD_NAME_LED_GREEN,
                                 pcCurrentGreenLedState,
                                 shadowDemoSTATE_FIELD_NAME_LED_RED,
                                 pcCurrentRedLedState,
                                 shadowDemoSTATE_FIELD_NAME_LED_YELLOW,
                                 pcCurrentYellowLedState );
}

/*-----------------------------------------------------------*/


static void setGpio( const int num, const bool state )
{
    gpio_set_direction( num, GPIO_MODE_OUTPUT );
    gpio_set_level( num, state ? 1 : 0 );
}


/*
 * the state object should look like this:
    {
      "blue": "off",
      "green": "on",
      "red": "off",
      "yellow": "off"
    }
 */
static BaseType_t prvUpdateLed( const char * pcStateJsonString,
                                const char * pcNodeName,
                                const char * pcCurrentLedState,
                                const int gpio )
{
    BaseType_t xReturn;
    char pcNewLedState[shadowDemoBUFFER_LENGTH];

    xReturn = pdFALSE;

    if( prvExtractTopLevelJsonNode( pcNewLedState,
                                    shadowDemoBUFFER_LENGTH,
                                    pcStateJsonString,
                                    pcNodeName ) > 0 )
    {
        if( ( strcmp("on", pcNewLedState ) == 0 )
         || ( strcmp("off", pcNewLedState ) == 0 ) )
        {
            strcpy( ( char * )pcCurrentLedState, pcNewLedState );

            setGpio( gpio, strcmp("on", pcNewLedState ) == 0 );

            xReturn = pdTRUE;
        }
        else
        {
            configPRINTF( ( "unrecognized LED value '%s'\r\n", pcNewLedState ) );
        }
    }
    else
    {
        configPRINTF( ( "invalid state contents '%s'\r\n", pcStateJsonString ) );
    }

    return xReturn;
}


static BaseType_t prvDoStateUpdate( const char * pcStateJsonString, uint32_t ulStateJsonStringLength )
{
    prvUpdateLed(pcStateJsonString, shadowDemoSTATE_FIELD_NAME_LED_BLUE,   pcCurrentBlueLedState,   GPIO_NUM_21);
    prvUpdateLed(pcStateJsonString, shadowDemoSTATE_FIELD_NAME_LED_GREEN,  pcCurrentGreenLedState,  GPIO_NUM_5);
    prvUpdateLed(pcStateJsonString, shadowDemoSTATE_FIELD_NAME_LED_RED,    pcCurrentRedLedState,    GPIO_NUM_22);
    prvUpdateLed(pcStateJsonString, shadowDemoSTATE_FIELD_NAME_LED_YELLOW, pcCurrentYellowLedState, GPIO_NUM_4);

    return pdTRUE;
}

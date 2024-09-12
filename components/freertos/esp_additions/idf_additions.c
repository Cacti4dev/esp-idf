/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/message_buffer.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/portmacro.h"

/*
 * This file contains the implementation for some the functions in
 * idf_additions.h
 */

/* -----------------------------------------------------------------------------
 * Creation With Memory Caps
 * -------------------------------------------------------------------------- */

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

/* ---------------------------------- Tasks --------------------------------- */

    BaseType_t xTaskCreatePinnedToCoreWithCaps( TaskFunction_t pvTaskCode,
                                                const char * const pcName,
                                                const configSTACK_DEPTH_TYPE usStackDepth,
                                                void * const pvParameters,
                                                UBaseType_t uxPriority,
                                                TaskHandle_t * const pvCreatedTask,
                                                const BaseType_t xCoreID,
                                                UBaseType_t uxMemoryCaps )
    {
        TaskHandle_t xHandle;
        StaticTask_t * pxTaskBuffer;
        StackType_t * pxStack;

        /* Allocate memory for the task's TCB. We use pvPortMalloc() here as the
         * TCB must be in internal memory. */
        pxTaskBuffer = pvPortMalloc( sizeof( StaticTask_t ) );

        /* Allocate memory for the task's stack using the provided memory caps
         * */
        pxStack = heap_caps_malloc( usStackDepth, ( uint32_t ) uxMemoryCaps );

        if( ( pxTaskBuffer == NULL ) || ( pxStack == NULL ) )
        {
            goto err;
        }

        /* Create the task using static creation API*/
        xHandle = xTaskCreateStaticPinnedToCore( pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxStack, pxTaskBuffer, xCoreID );

        if( xHandle == NULL )
        {
            /* Failed to create task */
            goto err;
        }
        else if( pvCreatedTask != NULL )
        {
            /* Task created successfully. Return the task handle */
            *pvCreatedTask = xHandle;
        }

        return pdPASS;

err:
        heap_caps_free( pxStack );
        vPortFree( pxTaskBuffer );
        return pdFAIL;
    }

#endif /* if ( configSUPPORT_STATIC_ALLOCATION == 1 ) */
/*----------------------------------------------------------*/

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

    static void prvTaskDeleteWithCapsTask( void * pvParameters )
    {
        TaskHandle_t xTaskToDelete = ( TaskHandle_t ) pvParameters;

        /* The task to be deleted must not be running */
        configASSERT( eRunning != eTaskGetState( xTaskToDelete ) );

        /* Delete the WithCaps task */
        vTaskDeleteWithCaps( xTaskToDelete );

        /* Delete the temporary clean up task */
        vTaskDelete( NULL );
    }

    void vTaskDeleteWithCaps( TaskHandle_t xTaskToDelete )
    {
        /* THIS FUNCTION SHOULD NOT BE CALLED FROM AN INTERRUPT CONTEXT. */
        /*TODO: Update it to use portASSERT_IF_IN_ISR() instead. (IDF-10540) */
        vPortAssertIfInISR();

        TaskHandle_t xCurrentTaskHandle = xTaskGetCurrentTaskHandle();
        configASSERT( xCurrentTaskHandle != NULL );

        if( ( xTaskToDelete == NULL ) || ( xTaskToDelete == xCurrentTaskHandle ) )
        {
            /* The WithCaps task is deleting itself. While, the task can put itself on the
             * xTasksWaitingTermination list via the vTaskDelete() call, the idle
             * task will not free the task TCB and stack memories we created statically
             * during xTaskCreateWithCaps() or xTaskCreatePinnedToCoreWithCaps(). This
             * task will never be rescheduled once it is on the xTasksWaitingTermination
             * list and will not be able to clear the memories. Therefore, it will leak memory.
             *
             * To avoid this, we create a new "temporary clean up" task to delete the current task.
             * This task is created at the priority of the task to be deleted with the same core
             * affitinty. Its limited purpose is to delete the self-deleting task created WithCaps.
             *
             * This approach has the following problems -
             * 1. Once a WithCaps task deletes itself via vTaskDeleteWithCaps(), it may end up in the
             *    suspended tasks lists for a short time before being deleted. This can give an incorrect
             *    picture about the system state.
             *
             * 2. This approach is wasteful and can be error prone. The temporary clean up task will need
             *    system resources to get scheduled and cleanup the WithCaps task. It can be a problem if the system
             *    has several self-deleting WithCaps tasks.
             *
             * TODO: A better approach could be either -
             *
             * 1. Delegate memory management to the application/user. This way the kernel needn't bother about freeing
             *    the memory (like other static memory task creation APIs like xTaskCreateStatic()) (IDF-10521)
             *
             * 2. Have a post deletion hook/callback from the IDLE task to notify higher layers when it is safe to
             *    perform activities such as clearing up the TCB and stack memories. (IDF-10522) */
            if( xTaskCreatePinnedToCore( ( TaskFunction_t ) prvTaskDeleteWithCapsTask, "prvTaskDeleteWithCapsTask", configMINIMAL_STACK_SIZE, xCurrentTaskHandle, uxTaskPriorityGet( xTaskToDelete ), NULL, xPortGetCoreID() ) != pdFAIL )
            {
                /* Although the current task should get preemted immediately when prvTaskDeleteWithCapsTask is created,
                 * for safety, we suspend the current task and wait for prvTaskDeleteWithCapsTask to delete it. */
                vTaskSuspend( xTaskToDelete );

                /* Should never reach here */
                ESP_LOGE( "freertos_additions", "%s: Failed to suspend the task to be deleted", __func__ );
                abort();
            }
            else
            {
                /* Failed to create the task to delete the current task. */
                ESP_LOGE( "freertos_additions", "%s: Failed to create the task to delete the current task", __func__ );
                abort();
            }
        }

        #if ( configNUM_CORES > 1 )
            else if( eRunning == eTaskGetState( xTaskToDelete ) )
            {
                /* The WithCaps task is running on another core.
                 * We suspend the task first and then delete it. */
                vTaskSuspend( xTaskToDelete );

                /* Wait for the task to be suspended */
                while( eRunning == eTaskGetState( xTaskToDelete ) )
                {
                    portYIELD_WITHIN_API();
                }

                BaseType_t xResult;
                StaticTask_t * pxTaskBuffer;
                StackType_t * puxStackBuffer;

                xResult = xTaskGetStaticBuffers( xTaskToDelete, &puxStackBuffer, &pxTaskBuffer );
                configASSERT( xResult == pdTRUE );
                configASSERT( puxStackBuffer != NULL );
                configASSERT( pxTaskBuffer != NULL );

                /* Delete the task */
                vTaskDelete( xTaskToDelete );

                /* Free the memory buffers */
                heap_caps_free( puxStackBuffer );
                vPortFree( pxTaskBuffer );
            }
        #endif /* if ( configNUM_CORES > 1 ) */
        else
        {
            /* The WithCaps task is not running and is being deleted
             * from another task's context. */
            configASSERT( eRunning != eTaskGetState( xTaskToDelete ) );

            BaseType_t xResult;
            StaticTask_t * pxTaskBuffer;
            StackType_t * puxStackBuffer;

            xResult = xTaskGetStaticBuffers( xTaskToDelete, &puxStackBuffer, &pxTaskBuffer );
            configASSERT( xResult == pdTRUE );
            configASSERT( puxStackBuffer != NULL );
            configASSERT( pxTaskBuffer != NULL );

            /* We can delete the task and free the memory buffers. */
            vTaskDelete( xTaskToDelete );

            /* Free the memory buffers */
            heap_caps_free( puxStackBuffer );
            vPortFree( pxTaskBuffer );
        } /* if( ( xTaskToDelete == NULL ) || ( xTaskToDelete == xCurrentTaskHandle ) ) */
    }

/* ---------------------------------- Queue --------------------------------- */

    QueueHandle_t xQueueCreateWithCaps( UBaseType_t uxQueueLength,
                                        UBaseType_t uxItemSize,
                                        UBaseType_t uxMemoryCaps )
    {
        QueueHandle_t xQueue;
        StaticQueue_t * pxQueueBuffer;
        uint8_t * pucQueueStorageBuffer;

        /* Allocate memory for the queue using the provided memory caps */
        pxQueueBuffer = heap_caps_malloc( sizeof( StaticQueue_t ), ( uint32_t ) uxMemoryCaps );

        if( uxItemSize == 0 )
        {
            pucQueueStorageBuffer = NULL;
        }
        else
        {
            pucQueueStorageBuffer = heap_caps_malloc( uxQueueLength * uxItemSize, ( uint32_t ) uxMemoryCaps );
        }

        if( ( pxQueueBuffer == NULL ) || ( ( uxItemSize > 0 ) && ( pucQueueStorageBuffer == NULL ) ) )
        {
            goto err;
        }

        /* Create the queue using static creation API */
        xQueue = xQueueCreateStatic( uxQueueLength, uxItemSize, pucQueueStorageBuffer, pxQueueBuffer );

        if( xQueue == NULL )
        {
            goto err;
        }

        return xQueue;

err:
        heap_caps_free( pucQueueStorageBuffer );
        heap_caps_free( pxQueueBuffer );
        return NULL;
    }

    void vQueueDeleteWithCaps( QueueHandle_t xQueue )
    {
        BaseType_t xResult;
        StaticQueue_t * pxQueueBuffer;
        uint8_t * pucQueueStorageBuffer;

        /* Retrieve the buffers used to create the queue before deleting it */
        xResult = xQueueGetStaticBuffers( xQueue, &pucQueueStorageBuffer, &pxQueueBuffer );
        configASSERT( xResult == pdTRUE );

        /* Delete the queue */
        vQueueDelete( xQueue );

        /* Free the memory buffers */
        heap_caps_free( pxQueueBuffer );
        heap_caps_free( pucQueueStorageBuffer );
    }

/* -------------------------------- Semaphore ------------------------------- */

    SemaphoreHandle_t xSemaphoreCreateGenericWithCaps( UBaseType_t uxMaxCount,
                                                       UBaseType_t uxInitialCount,
                                                       const uint8_t ucQueueType,
                                                       UBaseType_t uxMemoryCaps )
    {
        SemaphoreHandle_t xSemaphore;
        StaticSemaphore_t * pxSemaphoreBuffer;

        /* Allocate memory for the semaphore using the provided memory caps */
        pxSemaphoreBuffer = heap_caps_malloc( sizeof( StaticSemaphore_t ), ( uint32_t ) uxMemoryCaps );

        if( pxSemaphoreBuffer == NULL )
        {
            return NULL;
        }

        /* Create the semaphore using static creation API */
        if( ucQueueType == queueQUEUE_TYPE_MUTEX )
        {
            xSemaphore = xSemaphoreCreateMutexStatic( pxSemaphoreBuffer );
        }
        else if( ucQueueType == queueQUEUE_TYPE_COUNTING_SEMAPHORE )
        {
            xSemaphore = xSemaphoreCreateCountingStatic( uxMaxCount, uxInitialCount, pxSemaphoreBuffer );
        }
        else if( ucQueueType == queueQUEUE_TYPE_BINARY_SEMAPHORE )
        {
            xSemaphore = xSemaphoreCreateBinaryStatic( pxSemaphoreBuffer );
        }
        else /* ucQueueType == queueQUEUE_TYPE_RECURSIVE_MUTEX */
        {
            xSemaphore = xSemaphoreCreateRecursiveMutexStatic( pxSemaphoreBuffer );
        }

        if( xSemaphore == NULL )
        {
            heap_caps_free( pxSemaphoreBuffer );
        }

        return xSemaphore;
    }

    void vSemaphoreDeleteWithCaps( SemaphoreHandle_t xSemaphore )
    {
        BaseType_t xResult;
        StaticSemaphore_t * pxSemaphoreBuffer;

        /* Retrieve the buffer used to create the semaphore before deleting it
         * */
        xResult = xSemaphoreGetStaticBuffer( xSemaphore, &pxSemaphoreBuffer );
        configASSERT( xResult == pdTRUE );

        /* Delete the semaphore */
        vSemaphoreDelete( xSemaphore );

        /* Free the memory buffer */
        heap_caps_free( pxSemaphoreBuffer );
    }

/* ------------------------- Stream & Message Buffers ----------------------- */

    StreamBufferHandle_t xStreamBufferGenericCreateWithCaps( size_t xBufferSizeBytes,
                                                             size_t xTriggerLevelBytes,
                                                             BaseType_t xIsMessageBuffer,
                                                             UBaseType_t uxMemoryCaps )
    {
        StreamBufferHandle_t xStreamBuffer;
        StaticStreamBuffer_t * pxStaticStreamBuffer;
        uint8_t * pucStreamBufferStorageArea;

        /* Allocate memory for the stream or message buffer using the provided
         * memory caps */
        pxStaticStreamBuffer = heap_caps_malloc( sizeof( StaticStreamBuffer_t ), ( uint32_t ) uxMemoryCaps );
        pucStreamBufferStorageArea = heap_caps_malloc( xBufferSizeBytes, ( uint32_t ) uxMemoryCaps );

        if( ( pxStaticStreamBuffer == NULL ) || ( pucStreamBufferStorageArea == NULL ) )
        {
            goto err;
        }

        /* Create the stream or message buffer using static creation API */
        if( xIsMessageBuffer == pdTRUE )
        {
            xStreamBuffer = ( StreamBufferHandle_t ) xMessageBufferCreateStatic( xBufferSizeBytes, pucStreamBufferStorageArea, pxStaticStreamBuffer );
        }
        else
        {
            xStreamBuffer = xStreamBufferCreateStatic( xBufferSizeBytes, xTriggerLevelBytes, pucStreamBufferStorageArea, pxStaticStreamBuffer );
        }

        if( xStreamBuffer == NULL )
        {
            goto err;
        }

        return xStreamBuffer;

err:
        heap_caps_free( pucStreamBufferStorageArea );
        heap_caps_free( pxStaticStreamBuffer );
        return NULL;
    }

    void vStreamBufferGenericDeleteWithCaps( StreamBufferHandle_t xStreamBuffer,
                                             BaseType_t xIsMessageBuffer )
    {
        BaseType_t xResult;
        StaticStreamBuffer_t * pxStaticStreamBuffer;
        uint8_t * pucStreamBufferStorageArea;

        /* Retrieve the buffers used to create the stream or message buffer
         * before deleting it */
        if( xIsMessageBuffer == pdTRUE )
        {
            xResult = xMessageBufferGetStaticBuffers( xStreamBuffer, &pucStreamBufferStorageArea, &pxStaticStreamBuffer );
        }
        else
        {
            xResult = xStreamBufferGetStaticBuffers( xStreamBuffer, &pucStreamBufferStorageArea, &pxStaticStreamBuffer );
        }

        configASSERT( xResult == pdTRUE );

        /* Delete the stream or message buffer */
        if( xIsMessageBuffer == pdTRUE )
        {
            vMessageBufferDelete( xStreamBuffer );
        }
        else
        {
            vSemaphoreDelete( xStreamBuffer );
        }

        /* Free the memory buffers */
        heap_caps_free( pxStaticStreamBuffer );
        heap_caps_free( pucStreamBufferStorageArea );
    }

/* ------------------------------ Event Groups ------------------------------ */

    EventGroupHandle_t xEventGroupCreateWithCaps( UBaseType_t uxMemoryCaps )
    {
        EventGroupHandle_t xEventGroup;
        StaticEventGroup_t * pxEventGroupBuffer;

        /* Allocate memory for the event group using the provided memory caps */
        pxEventGroupBuffer = heap_caps_malloc( sizeof( StaticEventGroup_t ), uxMemoryCaps );

        if( pxEventGroupBuffer == NULL )
        {
            return NULL;
        }

        /* Create the event group using static creation API */
        xEventGroup = xEventGroupCreateStatic( pxEventGroupBuffer );

        if( xEventGroup == NULL )
        {
            heap_caps_free( pxEventGroupBuffer );
        }

        return xEventGroup;
    }

    void vEventGroupDeleteWithCaps( EventGroupHandle_t xEventGroup )
    {
        BaseType_t xResult;
        StaticEventGroup_t * pxEventGroupBuffer;

        /* Retrieve the buffer used to create the event group before deleting it
         * */
        xResult = xEventGroupGetStaticBuffer( xEventGroup, &pxEventGroupBuffer );
        configASSERT( xResult == pdTRUE );

        /* Delete the event group */
        vEventGroupDelete( xEventGroup );

        /* Free the memory buffer */
        heap_caps_free( pxEventGroupBuffer );
    }

#endif /* if ( configSUPPORT_STATIC_ALLOCATION == 1 ) */

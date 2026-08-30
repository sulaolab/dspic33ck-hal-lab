/*******************************************************************************
Copyright 2016 Microchip Technology Inc. (www.microchip.com)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*******************************************************************************/

/*
 * Modified by SulaoLab, 2026 (Apache-2.0 section 4(b)). Changes to Microchip's
 * original: the clock frequencies this file needs come from the Clock HAL
 * definitions instead of being hardcoded here, and the 1 ms tick is driven
 * through the CK timer HAL rather than owning Timer1 directly. Cadences are
 * stated in milliseconds. Relocated from firmware.X/bsp/ to src/app/.
 */

#include <xc.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "timer_1ms.h"
#include "timer_app.h"

/* Compiler checks and configuration *******************************/
#ifndef TIMER_MAX_1MS_CLIENTS
    #define TIMER_MAX_1MS_CLIENTS 10
#endif

/*
 * WHAT LEFT THIS FILE ON 2026-08-03, and why it is not a loss:
 *
 *   _T1Interrupt              -> app/timer_app.c. This file is EXCLUDED from the
 *       EV88G73A configuration, so while it carried the only Timer1 vector in the tree,
 *       that board could not run a 1 ms tick at all -- the vector belonged to nobody.
 *       The per-millisecond client walk below is now reached through
 *       timer_app_set_tick_hook(), i.e. this file keeps its registry and gives up the
 *       vector.
 *   the tick timer configuration -> app/timer_app.c as well, including its choice of
 *       FRC at 8 MHz (which is what DM330030 still asks for, via
 *       timer_app_start_from_frc() in its profile_bring_up) and its IRQ priority, which
 *       CHANGED from 7 to 2 in the move. 7 put the millisecond tick above the TDM block
 *       ISR at 4; the reason that matters is recorded in timer_app.c.
 *
 * What is left is the vendor BSP contract and nothing else: TICK_HANDLER registration,
 * cancellation, and the walk. TIMER_SetConfiguration() no longer starts the timer -- the
 * profile does, before this -- so it now REPORTS whether the running time is available
 * rather than establishing it. Its callers already treat false as "no ticks for you".
 */

/* Type Definitions ************************************************/
typedef struct
{
    TICK_HANDLER handle;
    uint32_t rate;
    uint32_t count;
} TICK_REQUEST;

/* Variables *******************************************************/
static TICK_REQUEST requests[TIMER_MAX_1MS_CLIENTS];
static bool configured = false;

/*********************************************************************
* Function: void TIMER_CancelTick(TICK_HANDLER handle)
*
* Overview: Cancels a tick request.
*
* PreCondition: None
*
* Input:  handle - the function that was handling the tick request
*
* Output: None
*
********************************************************************/
void TIMER_CancelTick(TICK_HANDLER handle)
{
    uint8_t i;

    for(i = 0; i < TIMER_MAX_1MS_CLIENTS; i++)
    {
        if(requests[i].handle == handle)
        {
            requests[i].handle = NULL;
        }
    }
}

/*********************************************************************
 * Function: bool TIMER_RequestTick(TICK_HANDLER handle, uint32_t rate)
 *
 * Overview: Requests to receive a periodic event.
 *
 * PreCondition: None
 *
 * Input:  handle - the function that will be called when the time event occurs
 *         rate - the number of ticks per event.
 *
 * Output: bool - true if successful, false if unsuccessful
 *
 ********************************************************************/
bool TIMER_RequestTick ( TICK_HANDLER handle , uint32_t rate )
{
    uint8_t i;
	
    if(configured == false)
    {
        return false;
    }

    for(i = 0; i < TIMER_MAX_1MS_CLIENTS; i++)
    {
        if(requests[i].handle == NULL)
        {
            requests[i].handle = handle;
            requests[i].rate = rate;
            requests[i].count = 0;

            return true;
        }
    }

    return false;
}

/*********************************************************************
 * Function: bool TIMER_SetConfiguration(TIMER_CONFIGURATIONS configuration)
 *
 * Overview: Initializes the timer.
 *
 * PreCondition: None
 *
 * Input:  None
 *
 * Output: bool - true if successful, false if unsuccessful
 *
 ********************************************************************/
static void timer_1ms_service_clients(void);

bool TIMER_SetConfiguration ( TIMER_CONFIGURATIONS configuration )
{
    switch(configuration)
    {
        case TIMER_CONFIGURATION_1MS:
        {
            //Clear any registered timer callback function requests
            memset(requests, 0, sizeof(requests));

            /*
             * The RUNNING TIME IS NOT THIS FILE'S TO START any more -- the profile does
             * that in profile_bring_up(), so that a board with no vendor BSP layer
             * compiled in still has one. What is left to do here is check that it IS
             * running, because every client this registry serves is paced by it: saying
             * true with a standing-still counter would hand out ticks that never arrive.
             */
            if(!timer_app_running())
            {
                configured = false;
                return false;
            }

            timer_app_set_tick_hook(timer_1ms_service_clients);
            configured = true;
            return true;
        }

        case TIMER_CONFIGURATION_OFF:
            /*
             * Detach from the tick and forget the clients -- but LEAVE THE TIMER RUNNING.
             * It is the whole application's running time now, not this layer's private
             * timer, and every GetTicks()-paced cadence in the tree would stop with it.
             * The vendor contract this implements is "stop delivering ticks to my
             * clients", and that is exactly what detaching does.
             */
            timer_app_set_tick_hook(NULL);
            memset(requests, 0, sizeof(requests));
            configured = false;
            return true;
    }

    return false;
}

/*
 * Service each registered 1 ms client. Called from app/timer_app.c's _T1Interrupt via
 * timer_app_set_tick_hook(), which is where the vector lives; the tick counter itself has
 * already been advanced by the time this runs.
 */
static void timer_1ms_service_clients( void )
{
    uint8_t i;

    for(i = 0; i < TIMER_MAX_1MS_CLIENTS; i++)
    {
        if(requests[i].handle != NULL)
        {
            requests[i].count++;

            if(requests[i].count == requests[i].rate)
            {
                requests[i].handle();
                requests[i].count = 0;
            }
        }
    }
}

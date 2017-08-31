/*
 * Copyright (c) 2016, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*****************************************************************************

 Application Name     -   MQTT Client
 Application Overview -   The device is running a MQTT client which is
                           connected to the online broker. Three LEDs on the 
                           device can be controlled from a web client by
                           publishing msg on appropriate topics. Similarly, 
                           message can be published on pre-configured topics
                           by pressing the switch buttons on the device.
 
 Application Details  - Refer to 'MQTT Client' README.html
 
 *****************************************************************************/
//*****************************************************************************
//
//! \addtogroup mqtt_server
//! @{
//
//*****************************************************************************
/* Standard includes                                                          */
#include <stdlib.h>

/* Hardware includes                                                          */
#include <ti/devices/cc32xx/inc/hw_types.h>
#include <ti/devices/cc32xx/inc/hw_ints.h>
#include <ti/devices/cc32xx/inc/hw_memmap.h>

/* Driverlib includes                                                         */
#include <ti/devices/cc32xx/driverlib/rom.h>
#include <ti/devices/cc32xx/driverlib/rom_map.h>
#include <ti/devices/cc32xx/driverlib/timer.h>

/* TI-Driver includes                                                         */
#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>

/* Simplelink includes                                                        */
#include <ti/drivers/net/wifi/simplelink.h>


/* Common interface includes                                                  */
#include "network_if.h"
#include "uart_term.h"

/* Application includes                                                       */
#include "Board.h"
#include "client_cbs.h"
#include "pthread.h"
#include "mqueue.h"
#include "time.h"
#include "unistd.h"


/* enables client authentication by the server                                */
//#define CLNT_USR_PWD

#define CLIENT_INIT_STATE        (0x01)
#define MQTT_INIT_STATE          (0x04)

#define APPLICATION_VERSION      "1.1.1"
#define APPLICATION_NAME         "MQTT client"


/* Spawn task priority and Task and Thread Stack Size                         */
#define TASKSTACKSIZE            2048
#define SPAWN_TASK_PRIORITY      9

/* secured client requires time configuration, in order to verify server      */
/* certificate validity (date).                                               */

/* Day of month (DD format) range 1-31                                        */
#define DAY                      1
/* Month (MM format) in the range of 1-12                                     */
#define MONTH                    5
/* Year (YYYY format)                                                         */
#define YEAR                     2017
/* Hours in the range of 0-23                                                 */
#define HOUR                     12
/* Minutes in the range of 0-59                                               */
#define MINUTES                  33
/* Seconds in the range of 0-59                                               */
#define SEC                      21   

/* Number of files used for secure connection                                 */
#define CLIENT_NUM_SECURE_FILES  1

/* Expiration value for the timer that is being used to toggle the Led.       */
#define TIMER_EXPIRATION_VALUE   100 * 1000000


//*****************************************************************************
//                 GLOBAL VARIABLES
//*****************************************************************************

/* Connection state: (0) - connected, (negative) - disconnected               */
int32_t gApConnectionState = -1;
uint32_t gInitState = 0;
bool     gResetApplication = false;
unsigned short g_usTimerInts;

/* AP Security Parameters                                                     */
SlWlanSecParams_t SecurityParams = { 0 };

/* Client ID                                                                  */
/* If ClientId isn't set, the MAC address of the device will be copied into   */
/* the ClientID parameter.                                                    */
char ClientId[13] = {'\0'};


timer_t g_timer;


/* Printing new line                                                          */
char lineBreak[] = "\n\r";

//*****************************************************************************
//                 Banner VARIABLES
//*****************************************************************************



//*****************************************************************************
//
//! Periodic Timer Interrupt Handler
//!
//! \param None
//!
//! \return None
//
//*****************************************************************************
void TimerPeriodicIntHandler(sigval val)
{
    /* Increment our interrupt counter.                                       */
    g_usTimerInts++;

    if (!(g_usTimerInts & 0x1))
    {
        /* Turn Led Off                                                       */
        GPIO_write(Board_LED0, Board_LED_OFF);
    }
    else
    {
        /* Turn Led On                                                        */
        GPIO_write(Board_LED0, Board_LED_ON);
    }
}

//*****************************************************************************
//
//! Function to configure and start timer to blink the LED while device is
//! trying to connect to an AP
//!
//! \param none
//!
//! return none
//
//*****************************************************************************
void LedTimerConfigNStart()
{
    struct itimerspec value;
    sigevent sev;

    /* Create Timer                                                           */
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_notify_function = &TimerPeriodicIntHandler;
    timer_create(2, &sev, &g_timer);

    /* start timer                                                            */
    value.it_interval.tv_sec = 0;
    value.it_interval.tv_nsec = TIMER_EXPIRATION_VALUE;
    value.it_value.tv_sec = 0;
    value.it_value.tv_nsec = TIMER_EXPIRATION_VALUE;

    timer_settime(g_timer, 0, &value, NULL);
}

//*****************************************************************************
//
//! Disable the LED blinking Timer as Device is connected to AP
//!
//! \param none
//!
//! return none
//
//*****************************************************************************
void LedTimerDeinitStop()
{ 
    /* Disable the LED blinking Timer as Device is connected to AP.           */
    timer_delete(g_timer);
}




//*****************************************************************************
//
//! This function connect the MQTT device to an AP with the SSID which was 
//! configured in SSID_NAME definition which can be found in Network_if.h file, 
//! if the device can't connect to to this AP a request from the user for other 
//! SSID will appear.
//!
//! \param  none
//!
//! \return None
//!
//*****************************************************************************
int32_t HTTP_IF_Connect()
{
    int32_t lRetVal;
    char SSID_Remote_Name[32];
    int8_t Str_Length;

    memset(SSID_Remote_Name, '\0', sizeof(SSID_Remote_Name));
    Str_Length = strlen(SSID_NAME);

    if (Str_Length)
    {
        /* Copy the Default SSID to the local variable                        */
        strncpy(SSID_Remote_Name, SSID_NAME, Str_Length);
    }


    GPIO_write(Board_LED0, Board_LED_OFF);
    GPIO_write(Board_LED1, Board_LED_OFF);
    GPIO_write(Board_LED2, Board_LED_OFF);

    /* Reset The state of the machine                                         */
    Network_IF_ResetMCUStateMachine();

    /* Start the driver                                                       */
    lRetVal = Network_IF_InitDriver(ROLE_STA);
    if (lRetVal < 0)
    {
        UART_PRINT("Failed to start SimpleLink Device\n\r", lRetVal);
        return -1;
    }

    /* switch on Green LED to indicate Simplelink is properly up.             */
    GPIO_write(Board_LED2, Board_LED_ON);

    /* Start Timer to blink Red LED till AP connection                        */
    LedTimerConfigNStart();

    /* Initialize AP security params                                          */
    SecurityParams.Key = (signed char *) SECURITY_KEY;
    SecurityParams.KeyLen = strlen(SECURITY_KEY);
    SecurityParams.Type = SECURITY_TYPE;

    /* Connect to the Access Point                                            */
    lRetVal = Network_IF_ConnectAP(SSID_Remote_Name, SecurityParams);
    if (lRetVal < 0)
    {
        UART_PRINT("Connection to an AP failed\n\r");
        return -1;
    }

    /* Disable the LED blinking Timer as Device is connected to AP.           */
    LedTimerDeinitStop();

    /* Switch ON RED LED to indicate that Device acquired an IP.              */
    GPIO_write(Board_LED0, Board_LED_ON);

    sleep(1);

    GPIO_write(Board_LED0, Board_LED_OFF);
    GPIO_write(Board_LED1, Board_LED_OFF);
    GPIO_write(Board_LED2, Board_LED_OFF);

    return 0;
}


//*****************************************************************************
//!
//! Utility function which prints the borders
//!
//! \param[in] ch  -  hold the charater for the border.
//! \param[in] n   -  hold the size of the border.
//!
//! \return none.
//!
//*****************************************************************************

void printBorder(char ch, int n)
{
    int        i = 0;

    for(i=0; i<n; i++)    putch(ch);
}

//*****************************************************************************
//!
//! Set the ClientId with its own mac address
//! This routine converts the mac address which is given
//! by an integer type variable in hexadecimal base to ASCII
//! representation, and copies it into the ClientId parameter.
//!
//! \param  macAddress  -   Points to string Hex.
//!
//! \return void.
//!
//*****************************************************************************
void SetClientIdNamefromMacAddress(uint8_t *macAddress)
{
    uint8_t Client_Mac_Name[2];
    uint8_t Index;

    /* When ClientID isn't set, use the mac address as ClientID               */
    if (ClientId[0] == '\0')
    {
        /* 6 bytes is the length of the mac address                           */
        for (Index = 0; Index < SL_MAC_ADDR_LEN; Index++)
        {
            /* Each mac address byte contains two hexadecimal characters      */
            /* Copy the 4 MSB - the most significant character                */
            Client_Mac_Name[0] = (macAddress[Index] >> 4) & 0xf;
            /* Copy the 4 LSB - the least significant character               */
            Client_Mac_Name[1] = macAddress[Index] & 0xf;

            if (Client_Mac_Name[0] > 9)
            {
                /* Converts and copies from number that is greater than 9 in  */
                /* hexadecimal representation (a to f) into ascii character   */
                ClientId[2 * Index] = Client_Mac_Name[0] + 'a' - 10;
            }
            else
            {
                /* Converts and copies from number 0 - 9 in hexadecimal       */
                /* representation into ascii character                        */
                ClientId[2 * Index] = Client_Mac_Name[0] + '0';
            }
            if (Client_Mac_Name[1] > 9)
            {
                /* Converts and copies from number that is greater than 9 in  */
                /* hexadecimal representation (a to f) into ascii character   */
                ClientId[2 * Index + 1] = Client_Mac_Name[1] + 'a' - 10;
            }
            else
            {
                /* Converts and copies from number 0 - 9 in hexadecimal       */
                /* representation into ascii character                        */
                ClientId[2 * Index + 1] = Client_Mac_Name[1] + '0';
            }
        }
    }
}

//*****************************************************************************
//!
//! Utility function which Display the app banner
//!
//! \param[in] appName     -  holds the application name.
//! \param[in] appVersion  -  holds the application version.
//!
//! \return none.
//!
//*****************************************************************************

int32_t DisplayAppBanner(char* appName, char* appVersion)
{
    int32_t            ret = 0;
    uint8_t            macAddress[SL_MAC_ADDR_LEN];
    uint16_t           macAddressLen = SL_MAC_ADDR_LEN;
    uint16_t           ConfigSize = 0;
    uint8_t            ConfigOpt = SL_DEVICE_GENERAL_VERSION;
    SlDeviceVersion_t  ver = {0};

    ConfigSize = sizeof(SlDeviceVersion_t);

    /* Print device version info. */
    ret = sl_DeviceGet(SL_DEVICE_GENERAL, &ConfigOpt, &ConfigSize, (uint8_t*)(&ver));

    /* Print device Mac address */
    ret = sl_NetCfgGet(SL_NETCFG_MAC_ADDRESS_GET, 0, &macAddressLen, &macAddress[0]);

    UART_PRINT(lineBreak);
    UART_PRINT("\t");
    printBorder('=', 44);
    UART_PRINT(lineBreak);
    UART_PRINT("\t   %s Example Ver: %s",appName, appVersion);
    UART_PRINT(lineBreak);
    UART_PRINT("\t");
    printBorder('=', 44);
    UART_PRINT(lineBreak);
    UART_PRINT(lineBreak);
    UART_PRINT("\t CHIP: 0x%x",ver.ChipId);
    UART_PRINT(lineBreak);
    UART_PRINT("\t MAC:  %d.%d.%d.%d",ver.FwVersion[0],ver.FwVersion[1],ver.FwVersion[2],ver.FwVersion[3]);
    UART_PRINT(lineBreak);
    UART_PRINT("\t PHY:  %d.%d.%d.%d",ver.PhyVersion[0],ver.PhyVersion[1],ver.PhyVersion[2],ver.PhyVersion[3]);
    UART_PRINT(lineBreak);
    UART_PRINT("\t NWP:  %d.%d.%d.%d",ver.NwpVersion[0],ver.NwpVersion[1],ver.NwpVersion[2],ver.NwpVersion[3]);
    UART_PRINT(lineBreak);
    UART_PRINT("\t ROM:  %d",ver.RomVersion);
    UART_PRINT(lineBreak);
    UART_PRINT("\t HOST: %s", SL_DRIVER_VERSION);
    UART_PRINT(lineBreak);
    UART_PRINT("\t MAC address: %02x:%02x:%02x:%02x:%02x:%02x", macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
    UART_PRINT(lineBreak);
    UART_PRINT(lineBreak);
    UART_PRINT("\t");
    printBorder('=', 44);
    UART_PRINT(lineBreak);
    UART_PRINT(lineBreak);

    SetClientIdNamefromMacAddress(macAddress);

    return ret;
}

void mainThread(void * args)
{

    uint32_t count = 0;
    pthread_t spawn_thread = (pthread_t) NULL;
    pthread_attr_t pAttrs_spawn;
    struct sched_param priParam;
    int32_t retc = 0;
    UART_Handle tUartHndl;

    Board_initGPIO();
    Board_initSPI();

    /* Configure the UART                                                     */
    tUartHndl = InitTerm();
    /* remove uart receive from LPDS dependency                               */
    UART_control(tUartHndl, UART_CMD_RXDISABLE, NULL);

    /* Create the sl_Task                                                     */
    pthread_attr_init(&pAttrs_spawn);
    priParam.sched_priority = SPAWN_TASK_PRIORITY;
    retc = pthread_attr_setschedparam(&pAttrs_spawn, &priParam);
    retc |= pthread_attr_setstacksize(&pAttrs_spawn, TASKSTACKSIZE);
    retc |= pthread_attr_setdetachstate(&pAttrs_spawn, PTHREAD_CREATE_DETACHED);

    retc = pthread_create(&spawn_thread, &pAttrs_spawn, sl_Task, NULL);

    if (retc != 0)
    {
        UART_PRINT("could not create simplelink task\n\r");
        while (1);
    }

    retc = sl_Start(0, 0, 0);
    if (retc < 0)
    {
        /* Handle Error */
        UART_PRINT("\n sl_Start failed\n");
        while(1);
    }

    /* Output device information to the UART terminal */
    retc = DisplayAppBanner(APPLICATION_NAME, APPLICATION_VERSION);

    retc = sl_Stop(SL_STOP_TIMEOUT );
    if (retc < 0)
    {
        /* Handle Error */
        UART_PRINT("\n sl_Stop failed\n");
        while(1);
    }

    if(retc < 0)
    {
        /* Handle Error */
        UART_PRINT("mqtt_client - Unable to retrieve device information \n");
        while (1);
    }


    while (1)
    {

        gResetApplication = false;
        gInitState = 0;

        /* Connect to AP                                                      */
        gApConnectionState = HTTP_IF_Connect();

        gInitState |= MQTT_INIT_STATE;

        /* Wait for init to be completed!!!                                   */
        while (gInitState != 0)
        {
            UART_PRINT(".");
            sleep(1);
        }
        UART_PRINT(".\r\n");

        while (gResetApplication == false);

        UART_PRINT("TO Complete - Closing all threads and resources\r\n");


        UART_PRINT("reopen MQTT # %d  \r\n", ++count);

    }
}

//*****************************************************************************
//
// Close the Doxygen group.
//! @}
//
//*****************************************************************************

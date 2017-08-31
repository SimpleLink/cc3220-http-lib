//*****************************************************************************
//
// Copyright (C) 2014 Texas Instruments Incorporated - http://www.ti.com/ 
// 
// 
//  Redistribution and use in source and binary forms, with or without 
//  modification, are permitted provided that the following conditions 
//  are met:
//
//    Redistributions of source code must retain the above copyright 
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the 
//    documentation and/or other materials provided with the   
//    distribution.
//
//    Neither the name of Texas Instruments Incorporated nor the names of
//    its contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
//  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
//  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
//  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
//  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
//  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
//  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//*****************************************************************************

//*****************************************************************************
//
// Application Name     - Get Weather
// Application Overview - Get Weather application connects to "openweathermap.org"
//                        server, requests for weather details of the specified
//                        city, process data and displays it on the Hyperterminal.
//
// Application Details  -
// http://processors.wiki.ti.com/index.php/CC32xx_Info_Center_Get_Weather_Application
// or
// docs\examples\CC32xx_Info_Center_Get_Weather_Application.pdf
//
//*****************************************************************************


//****************************************************************************
//
//! \addtogroup get_weather
//! @{
//
//****************************************************************************

// Standard includes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hardware includes                                                          */
#include <ti/devices/cc32xx/inc/hw_types.h>
#include <ti/devices/cc32xx/inc/hw_ints.h>
#include <ti/devices/cc32xx/inc/hw_memmap.h>

/* Driverlib includes                                                         */
#include <ti/devices/cc32xx/driverlib/rom.h>
#include <ti/devices/cc32xx/driverlib/rom_map.h>
#include <ti/devices/cc32xx/driverlib/timer.h>
#include <ti/devices/cc32xx/driverlib/utils.h>

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


// HTTP Client lib
#include <ti/net/http/client/httpcli.h>
#include <ti/net/http/client/common.h>

//****************************************************************************
//                          LOCAL DEFINES                                   
//****************************************************************************
#define APPLICATION_VERSION     "1.1.1"
#define APPLICATION_NAME        "Get Weather"

#define SLEEP_TIME              8000000
#define SUCCESS                 0

#define PREFIX_BUFFER "/data/2.5/weather?q="
#define POST_BUFFER     "&mode=xml&units=imperial&APPID=462cb151c0193addbe2d45c3ed4860e9 HTTP/1.1\r\nHost:api.openweathermap.org\r\nAccept: */"

#define HOST_NAME       "api.openweathermap.org"
#define HOST_PORT       (80)

#define TASKSTACKSIZE            4096
#define SPAWN_TASK_PRIORITY      9
#define HTTP_INIT_STATE          (0x04)

//*****************************************************************************
//                 GLOBAL VARIABLES -- Start
//*****************************************************************************
// Application specific status/error codes
typedef enum{
    // Choosing -0x7D0 to avoid overlap w/ host-driver's error codes
    SERVER_GET_WEATHER_FAILED = -0x7D0,
    WRONG_CITY_NAME = SERVER_GET_WEATHER_FAILED - 1,
    NO_WEATHER_DATA = WRONG_CITY_NAME - 1,
    DNS_LOOPUP_FAILED = WRONG_CITY_NAME  -1,

    STATUS_CODE_MAX = -0xBB8
}e_AppStatusCodes;

#define TIMER_EXPIRATION_VALUE   100 * 1000000



unsigned short g_usTimerInts;
timer_t g_timer;
SlWlanSecParams_t SecurityParams = {0};  // AP Security Parameters

char acRecvbuff[1460];  // Buffer to receive data

/* Client ID                                                                  */
/* If ClientId isn't set, the MAC address of the device will be copied into   */
/* the ClientID parameter.                                                    */
char ClientId[13] = {'\0'};
uint32_t gInitState = 0;
bool     gResetApplication = false;
int32_t  gApConnectionState = -1;


/* Printing new line                                                          */
    char lineBreak[] = "\n\r";

#if defined(ccs)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif
//*****************************************************************************
//                 GLOBAL VARIABLES -- End
//*****************************************************************************


//****************************************************************************
//                      LOCAL FUNCTION PROTOTYPES                           
//****************************************************************************
static long GetWeather(HTTPCli_Handle cli, char *pcCityName);
void GetWeatherTask();
static int HandleXMLData(char *acRecvbuff);
static int FlushHTTPResponse(HTTPCli_Handle cli);


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

//****************************************************************************
//
//! Parsing XML data to dig wetaher information
//!
//! \param[in] acRecvbuff - XML Data
//!
//! return 0 on success else -ve
//
//****************************************************************************
static int HandleXMLData(char *acRecvbuff)
{
    char *pcIndxPtr;
    char *pcEndPtr;

    //
    // Get city name
    //
    pcIndxPtr = strstr(acRecvbuff, "name=");
    if(pcIndxPtr == NULL)
    {
        ASSERT_ON_ERROR(WRONG_CITY_NAME);
    }

    DBG_PRINT("\n\r****************************** \n\r\n\r");
    DBG_PRINT("City: ");
    if( NULL != pcIndxPtr )
    {
        pcIndxPtr = pcIndxPtr + strlen("name=") + 1;
        pcEndPtr = strstr(pcIndxPtr, "\">");
        if( NULL != pcEndPtr )
        {
           *pcEndPtr = 0;
        }
        DBG_PRINT("%s\n\r",pcIndxPtr);
    }
    else
    {
        DBG_PRINT("N/A\n\r");
        return NO_WEATHER_DATA;
    }

    //
    // Get temperature value
    //
    pcIndxPtr = strstr(pcEndPtr+1, "temperature value");
    DBG_PRINT("Temperature: ");
    if( NULL != pcIndxPtr )
    {
        pcIndxPtr = pcIndxPtr + strlen("temperature value") + 2;
        pcEndPtr = strstr(pcIndxPtr, "\" ");
        if( NULL != pcEndPtr )
        {
           *pcEndPtr = 0;
        }
        DBG_PRINT("%s\n\r",pcIndxPtr);
    }
    else
    {
        DBG_PRINT("N/A\n\r");
        return NO_WEATHER_DATA;
    }
    
    
    //
    // Get weather condition
    //
    pcIndxPtr = strstr(pcEndPtr+1, "weather number");
    DBG_PRINT("Weather Condition: ");
    if( NULL != pcIndxPtr )
    {
        pcIndxPtr = pcIndxPtr + strlen("weather number") + 14;
        pcEndPtr = strstr(pcIndxPtr, "\" ");
        if( NULL != pcEndPtr )
        {
           *pcEndPtr = 0;
        }
        DBG_PRINT("%s\n\r",pcIndxPtr);
    }
    else
    {
        DBG_PRINT("N/A\n\r");
        return NO_WEATHER_DATA;
    }

    return SUCCESS;
}

//****************************************************************************
//
//! This function flush received HTTP response
//!
//! \param[in] cli - HTTP client object
//!
//! return 0 on success else -ve
//
//****************************************************************************
static int FlushHTTPResponse(HTTPCli_Handle cli)
{
    const char *ids[2] = {
                            HTTPCli_FIELD_NAME_CONNECTION, /* App will get connection header value. all others will skip by lib */
                            NULL
                         };
    char buf[128];
    int id;
    int len = 1;
    bool moreFlag = 0;
    char ** prevRespFilelds = NULL;


    prevRespFilelds = HTTPCli_setResponseFields(cli, ids);

    //
    // Read response headers
    //
    while ((id = HTTPCli_getResponseField(cli, buf, sizeof(buf), &moreFlag))
            != HTTPCli_FIELD_ID_END)
    {

        if(id == 0)
        {
            if(!strncmp(buf, "close", sizeof("close")))
            {
                UART_PRINT("Connection terminated by server\n\r");
            }
        }

    }

    HTTPCli_setResponseFields(cli, (const char **)prevRespFilelds);

    while(1)
    {
        len = HTTPCli_readResponseBody(cli, buf, sizeof(buf) - 1, &moreFlag);
        ASSERT_ON_ERROR(len);

        if ((len - 2) >= 0 && buf[len - 2] == '\r' && buf [len - 1] == '\n')
        {

        }

        if(!moreFlag)
        {
            break;
        }
    }
    return SUCCESS;
}
char acSendBuff[512];   // Buffer to send data

//*****************************************************************************
//
//! GetWeather
//!
//! \brief  Obtaining the weather info for the specified city from the server
//!
//! \param  iSockID is the socket ID
//! \param  pcCityName is the pointer to the name of the city
//!
//! \return none.        
//!
//
//*****************************************************************************
static long GetWeather(HTTPCli_Handle cli, char *pcCityName)
{

    char* pcBufLocation;
    long retval = 0;
    int id;
    int len = 1;
    bool moreFlag = 0;
    char ** prevRespFilelds = NULL;
    HTTPCli_Field fields[2] = {
                                {HTTPCli_FIELD_NAME_HOST, HOST_NAME},
                                {NULL, NULL},
                              };
    const char *ids[3] = {
                            HTTPCli_FIELD_NAME_CONNECTION,
                            HTTPCli_FIELD_NAME_CONTENT_TYPE,
                            NULL
                         };
    //
    // Set request fields
    //
    HTTPCli_setRequestFields(cli, fields);

    pcBufLocation = acSendBuff;
    strcpy(pcBufLocation, PREFIX_BUFFER);
    pcBufLocation += strlen(PREFIX_BUFFER);
    strcpy(pcBufLocation , pcCityName);
    pcBufLocation += strlen(pcCityName);
    strcpy(pcBufLocation , POST_BUFFER);
    pcBufLocation += strlen(POST_BUFFER);
    *pcBufLocation = 0;

    UART_PRINT("\n\r");
    //
    // Make HTTP 1.1 GET request
    //
    retval = HTTPCli_sendRequest(cli, HTTPCli_METHOD_GET, acSendBuff, 0);
    if (retval < 0)
    {
        UART_PRINT("Failed to send HTTP 1.1 GET request.\n\r");
        return 	SERVER_GET_WEATHER_FAILED;
    }

    //
    // Test getResponseStatus: handle
    //
    retval = HTTPCli_getResponseStatus(cli);
    if (retval != 200)
    {
        UART_PRINT("HTTP Status Code: %d\r\n", retval);
        FlushHTTPResponse(cli);
        return SERVER_GET_WEATHER_FAILED;
    }

    prevRespFilelds = HTTPCli_setResponseFields(cli, ids);

    //
    // Read response headers
    //
    while ((id = HTTPCli_getResponseField(cli, acRecvbuff, sizeof(acRecvbuff), &moreFlag)) 
            != HTTPCli_FIELD_ID_END)
    {

        if(id == 0) // HTTPCli_FIELD_NAME_CONNECTION
        {
            if(!strncmp(acRecvbuff, "close", sizeof("close")))
            {
                UART_PRINT("Connection terminated by server.\n\r");
            }
        }
        else if(id == 1) // HTTPCli_FIELD_NAME_CONTENT_TYPE
        {
            UART_PRINT("Content Type: %s\r\n", acRecvbuff);
        }
    }

    HTTPCli_setResponseFields(cli, (const char **)prevRespFilelds);

    //
    // Read body
    //
    while (1)
    {
        len = HTTPCli_readResponseBody(cli, acRecvbuff, sizeof(acRecvbuff) - 1, &moreFlag);
        if(len < 0)
        {
            return SERVER_GET_WEATHER_FAILED;
        }
        acRecvbuff[len] = 0;

        if ((len - 2) >= 0 && acRecvbuff[len - 2] == '\r'
                && acRecvbuff [len - 1] == '\n')
        {
            break;
        }
        
        retval = HandleXMLData(acRecvbuff);
        ASSERT_ON_ERROR(retval);
        
        if(!moreFlag)
            break;

    }


    DBG_PRINT("\n\r****************************** \n\r");
    return SUCCESS;
}

int strIP(char *buff, unsigned long * pIP)
{
    return sprintf(buff, "%d.%d.%d.%d",
               SL_IPV4_BYTE(*pIP, 3),
               SL_IPV4_BYTE(*pIP, 2),
               SL_IPV4_BYTE(*pIP, 1),
               SL_IPV4_BYTE(*pIP, 0));
}

//****************************************************************************
//
//! Task function implementing the getweather functionality
//!
//! \param none
//! 
//! This function  
//!    1. Initializes the required peripherals
//!    2. Initializes network driver and connects to the default AP
//!    3. Creates a TCP socket, gets the server IP address using DNS
//!    4. Gets the weather info for the city specified
//!
//! \return None.
//
//****************************************************************************
int32_t HTTP_Connect(HTTPCli_Struct *pCli)
{
    int32_t retval;
    unsigned long ulDestinationIP;
    SlSockAddrIn_t addr;
    ;

    //
    // Get the serverhost IP address using the DNS lookup
    //
    retval = Network_IF_GetHostIP((char*)HOST_NAME, &ulDestinationIP);
    if(retval < 0)
    {
        UART_PRINT("DNS lookup failed. \n\r",retval);
        return retval;
    }

    //
    // Set up the input parameters for HTTP Connection
    //
    addr.sin_family = AF_INET;
    addr.sin_port = sl_Htons(HOST_PORT);
    addr.sin_addr.s_addr = sl_Htonl(ulDestinationIP);

    //
    // Testing HTTPCli open call: handle, address params only
    //
    HTTPCli_construct(pCli);
    retval = HTTPCli_connect(pCli, (struct sockaddr *)&addr, 0, NULL);
    if (retval < 0)
    {
        UART_PRINT("Failed to create instance of HTTP Client.\n\r");
        return retval;
    }    
    else
    {
        char buff[20];
        strIP(buff, &ulDestinationIP);
        UART_PRINT("HTTP Client is connected to %s (%s)\n\r", HOST_NAME, buff);
    }
    return retval;
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
int32_t WIFI_AP_Connect()
{
    int32_t retval;
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
    retval = Network_IF_InitDriver(ROLE_STA);
    if (retval < 0)
    {
        UART_PRINT("Failed to start SimpleLink Device\n\r", retval);
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
    retval = Network_IF_ConnectAP(SSID_Remote_Name, SecurityParams);
    if (retval < 0)
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
    int32_t retval;
    pthread_t spawn_thread = (pthread_t) NULL;
    pthread_attr_t pAttrs_spawn;
    struct sched_param priParam;
    int32_t retc = 0;
    UART_Handle tUartHndl;
    HTTPCli_Struct cli;

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

    gResetApplication = false;

    do
    {
        /* Connect to AP                                                      */
        retval = WIFI_AP_Connect();

    } while (retval != 0);

    while(1)
    {
        char acCityName[32];
        //
        // Get the city name over UART to get the weather info
        //
        UART_PRINT("\n\rEnter city name, or QUIT to quit: ");
        retval = GetCmd(acCityName, sizeof(acCityName));
        if(retval > 0)
        {
            if (!strcmp(acCityName,"QUIT") || !strcmp(acCityName,"quit"))
            {
                break;
            }
            else
            {
                /* Connect to HTTP Server */
                retval = HTTP_Connect(&cli);

                //
                // Get the weather info and display the same
                //
                if(retval == 0)
                {
                    retval = GetWeather(&cli, &acCityName[0]);

                    if(retval == SERVER_GET_WEATHER_FAILED)
                    {
                        UART_PRINT("Server Get Weather failed \n\r");
                    }
                    else if(retval == WRONG_CITY_NAME)
                    {
                        UART_PRINT("Wrong input\n\r");

                    }
                    else if(retval == NO_WEATHER_DATA)
                    {
                        UART_PRINT("Weather data not available\n\r");

                    }
                    else if(retval < 0)
                    {
                        UART_PRINT("unknown error (%d)\n\r", retval);

                    }
                }
                //
                // Wait a while before resuming
                //
                MAP_UtilsDelay(SLEEP_TIME);
            }
        }
    }

    HTTPCli_destruct(&cli);

    //
    // Stop the driver
    //
    retval = Network_IF_DeInitDriver();
    if(retval < 0)
    {
       UART_PRINT("Failed to stop SimpleLink Device\n\r");
       LOOP_FOREVER();
    }

    //
    // Switch Off RED & Green LEDs to indicate that Device is
    // disconnected from AP and Simplelink is shutdown
    //
    GPIO_write(Board_LED0, Board_LED_OFF);
    GPIO_write(Board_LED2, Board_LED_OFF);

    DBG_PRINT("GET_WEATHER: Test Complete\n\r");

    //
    // Loop here
    //
    LOOP_FOREVER();
}


//*****************************************************************************
//
// Close the Doxygen group.
//! @}
//
//*****************************************************************************

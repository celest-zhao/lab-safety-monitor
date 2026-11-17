/* control_task.c：平台通讯 / OC 协议 / 平台下发指令解析 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpio_api.h"
#include "rtc_api.h"
#include "usart_api.h"
#include "log.h"
#include "lcd.h"
#include "gui.h"
#include "net.h"
#include "ocprotocol.h"
#include "task_inter.h"
#include "error.h"
#include "view_task.h"
#include "control_task.h"
#include "sensor_task.h"
#include "offline_cache.h"    /* [离线缓存] 主轮询入口 */     /* Plat_SetSwitch / Plat_FillReport */
#include "led.h"

/* [编译期检查] 上行缓冲区 g_ptlSendBuf[50] 必须能装下整个协议结构体，
   以后谁再加字段，这里会直接编译报错，而不是悄悄写坏内存。 */
typedef char ptl_buf_size_check[(sizeof(PTL_DATA_STRUCT) <= sizeof(g_ptlSendBuf)) ? 1 : -1];

/* ---- 家居场景下行帧偏移（单位：hex 字符） ---- */
#define PTL_HOME_HDR          0x03u
#define PTL_FRAME_HEX_LEN     16
#define PTL_OFF_FAN           8
#define PTL_OFF_FAN_LEVEL     10
#define PTL_OFF_LED           14
#define PTL_VAL_ABSENT        0xFFu

UINT32 g_ControlTskHandle;

/******************************内部变量定义************************************/
uint16_t net_uart_len;      /* 串口数据长度变量 */
uint16_t debug_uart_len;    /* 串口数据长度变量 */

uint8_t *net_uart_buffer;   /* 串口数据接收数组 */
uint8_t *debug_uart_buffer; /* 串口数据接收数组 */

ViewDataStruct HomeView;    /* 显示状态结构体 */

/*******************************************************************************
* 名    称：Hex_Pair_To_U8
* 功    能：把 "0A" 这样的两字符十六进制转为数值
* 返 回 值：0 成功；-1 失败（含非十六进制字符或提前结束）
* 说    明：不使用 sscanf("%x")，避免 microlib 下的格式串开销与越界读取
*******************************************************************************/
static int8_t Hex_Pair_To_U8(const char *p, uint8_t *out)
{
    uint8_t hi, lo;

    if (out == NULL || p == NULL || p[0] == '\0' || p[1] == '\0')
    {
        return -1;
    }

    if      (p[0] >= '0' && p[0] <= '9') hi = (uint8_t)(p[0] - '0');
    else if (p[0] >= 'a' && p[0] <= 'f') hi = (uint8_t)(p[0] - 'a' + 10);
    else if (p[0] >= 'A' && p[0] <= 'F') hi = (uint8_t)(p[0] - 'A' + 10);
    else return -1;

    if      (p[1] >= '0' && p[1] <= '9') lo = (uint8_t)(p[1] - '0');
    else if (p[1] >= 'a' && p[1] <= 'f') lo = (uint8_t)(p[1] - 'a' + 10);
    else if (p[1] >= 'A' && p[1] <= 'F') lo = (uint8_t)(p[1] - 'A' + 10);
    else return -1;

    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

/* Key1 按键结束处理 */
void key1_end_handle(void)
{
}

/* Key2 按键结束处理 */
void key2_end_handle(void)
{
}

/* Key3 按键结束处理：调试打印开关 */
void key3_end_handle(void)
{
    debugflag = !debugflag;
}

/* Key4 按键结束处理：等待联网提示 */
void key4_end_handle(void)
{
    if (Is_Net_Module_Connected())
    {
        printf("请等待联网成功\r\n");
        return;
    }
}

/*******************************************************************************
* 名    称：Ptl_Data_Init
* 功    能：OC 通信协议初始化
* 说    明：Type=0x02 家居场景，与智能门禁平台一致；Ver="200"
*******************************************************************************/
void Ptl_Data_Init(void)
{
    memset(&g_pltSendData, 0, sizeof(PTL_DATA_STRUCT));

    g_pltSendData.Type = 0x02;      /* 家居场景 */
    g_pltSendData.Ver[0] = '2';
    g_pltSendData.Ver[1] = '0';
    g_pltSendData.Ver[2] = '0';     /* 固件版本 V2.0.0 */
}

/*******************************************************************************
* 名    称：read_uart_data
* 功    能：读取串口数据
*******************************************************************************/
void read_uart_data(void)
{
    net_uart_buffer = uart_read(Get_Net_Module_Uart_Port(), &net_uart_len);

    if ((net_uart_buffer != NULL) && (net_uart_len > 0))
    {
        Net_Module_Read_Data((char*)net_uart_buffer, net_uart_len);
        net_uart_len = 0;
        clearrxbuffer(Get_Net_Module_Uart_Port());
    }
    debug_uart_buffer = uart_read(DEBUG_UART, &debug_uart_len);
    if ((debug_uart_buffer != NULL) && (debug_uart_len > 0))
    {
        /* 按长度发送：数据里带 0x00 时 %s 会被截断 */
        uart_write(DEBUG_UART, debug_uart_len, debug_uart_buffer);
        uart_write(Get_Net_Module_Uart_Port(), debug_uart_len, debug_uart_buffer);
        clearrxbuffer(DEBUG_UART);
        debug_uart_len = 0;
    }
}

/*******************************************************************************
* 名    称：Get_Net_UI_Info
* 功    能：更新联网状态（LCD 由 sensor_task 独占，这里只缓存）
*******************************************************************************/
static void Get_Net_UI_Info(void)
{
    char* p = NULL;
    HomeView.state = Get_Net_Module_State();

    p = Get_Net_Module_UI_Line1();
    if (0 != p[0])
    {
        memset(HomeView.Line1, 0, sizeof(HomeView.Line1));
        strncpy((char*)HomeView.Line1, p, sizeof(HomeView.Line1));
    }

    p = Get_Net_Module_UI_Line2();
    if (0 != p[0])
    {
        memset(HomeView.Line2, 0, sizeof(HomeView.Line2));
        strncpy((char*)HomeView.Line2, p, sizeof(HomeView.Line2));
    }

    p = Get_Net_Module_UI_Line3();
    if (0 != p[0])
    {
        memset(HomeView.Line3, 0, sizeof(HomeView.Line3));
        strncpy((char*)HomeView.Line3, p, sizeof(HomeView.Line3));
    }
}

/*******************************************************************************
* 名    称：Task1_Deal
* 功    能：上行数据打包（心跳 5s 一次 / 事件立即触发）
* 说    明：必须先写好 Type/Ver 再 memcpy，否则会发出一帧旧场景类型的数据，
*          平台侧表现为“设备类型错乱 / 属性不刷新”。
*******************************************************************************/
void Task1_Deal(void)
{
    g_pltSendData.Type    = 0x02;   /* 家居场景 */
    g_pltSendData.Ver[0]  = 0x32;   /* '2' */
    g_pltSendData.Ver[1]  = 0x30;   /* '0' */
    g_pltSendData.Ver[2]  = 0x30;   /* '0' */

    /* 传感器量 + 执行器实际状态 -> 协议结构体（在 sensor_task 中实现） */
    Plat_FillReport(&g_pltSendData);

    g_ptlSendLen = sizeof(PTL_DATA_STRUCT);
    memcpy((uint8_t *)&g_ptlSendBuf[0], (uint8_t *)&g_pltSendData.Type, g_ptlSendLen);
}

/*******************************************************************************
* 名    称：ptl_Receive_OCData_Handle
* 功    能：平台下发指令处理
* 参数说明：data：家居场景十六进制串（16 字符，见文件头注释）
*******************************************************************************/
void ptl_Receive_OCData_Handle(char* data)
{
    uint8_t hdr = 0;
    uint8_t val = 0;
    int     len;
    uint8_t changed = 0;

    if (data == NULL)
    {
        return;
    }

    len = (int)strlen(data);
    if (len < PTL_FRAME_HEX_LEN)
    {
        printf("[PLAT] downlink frame too short (%d), ignored\r\n", len);
        return;
    }

    if (Hex_Pair_To_U8(&data[0], &hdr) != 0)
    {
        return;
    }
    if (hdr != PTL_HOME_HDR)
    {
        /* 非家居场景帧（例如残留的农业 0x06 帧），忽略且不改动任何状态 */
        return;
    }

    printf("[PLAT] cmd=%s\r\n", data);

    /* ---- 风扇状态 -> PA8 ---- */
    if (Hex_Pair_To_U8(&data[PTL_OFF_FAN], &val) == 0 && val != PTL_VAL_ABSENT)
    {
        Plat_SetSwitch(PLAT_SW_FAN, val ? 1 : 0);
        changed = 1;
    }

    /* ---- 家居照明 -> PB15 ---- */
    if (Hex_Pair_To_U8(&data[PTL_OFF_LED], &val) == 0 && val != PTL_VAL_ABSENT)
    {
        Plat_SetSwitch(PLAT_SW_LAMP, val ? 1 : 0);
        changed = 1;
    }

    if (changed)
    {
        /* 置事件标志：下一次 ptl_Heartbeat_Handle 立即上报最新状态 */
        Scene.event_sendflag = 1;
    }
}

/*******************************************************************************
* 名    称：task_control_init
* 功    能：控制初始化
*******************************************************************************/
void task_control_init(void)
{
    gpio_module_open(LEDS | BEEP);

    Net_Module_Init(NET_TYPE_WIFI);
    Error_Msg_Init();
    uart_open(DEBUG_UART, 115200);

    printf("\r\n  多传感器环境监测 · 智慧家居平台对接 V1.0  \r\n");
    printf("--------------------------------------------\r\n");
    printf("家居照明 -> PB15 照明继电器\r\n");
    printf("风扇状态 -> PA8  风扇继电器\r\n");
    printf("开始本实验：\r\n\r\n");

    Ptl_Data_Init();
}

/*******************************************************************************
* 名    称：control_task
* 功    能：通讯处理 + 网络状态 + 心跳
*******************************************************************************/
VOID control_task(VOID)
{
    UINT32 uwRet = LOS_OK;
    static uint32_t sys_time = 0;

    while (1)
    {
        ptl_Handle();          /* 通讯部分处理（发送队列 + 超时重发） */
        Error_Msg_Handle();

        if (HAL_GetTick() - sys_time > 1000)
        {
            sys_time = HAL_GetTick();
            /* 注意：LED1 归 sensor_task 的告警指示使用，此处不得再翻转，
               否则 PA4 会被两个任务争抢（旧版实验闪烁异常的原因）。 */
        }

        Get_Net_UI_Info();
        ptl_Heartbeat_Handle(); /* 5s 心跳 / 事件立即上报 */

        Net_Module_Handle();
        OfflineCache_Poll((uint8_t)Is_Net_Module_Connected());  /* [离线缓存] 网络三态/离线落盘/联网补传 */

        if (Scene.req_state == 1)
        {
            Scene.state = 0;    /* 状态变化已被消费 */
        }

        uwRet = LOS_TaskDelay(10);
        if (uwRet != LOS_OK)
        {
            return;
        }
    }
}

/*******************************************************************************
* 名    称：creat_control_task
*******************************************************************************/
UINT32 creat_control_task(VOID)
{
    UINT32 uwRet = LOS_OK;
    TSK_INIT_PARAM_S task_init_param = {0};   /* 未使用字段必须清零，避免取到栈上残留值 */

    task_init_param.usTaskPrio = 0;
    task_init_param.pcName = "control_task";
    task_init_param.pfnTaskEntry = (TSK_ENTRY_FUNC)control_task;
    task_init_param.uwStackSize = 0xA00;

    uwRet = LOS_TaskCreate(&g_ControlTskHandle, &task_init_param);
    if (LOS_OK != uwRet)
    {
        return uwRet;
    }
    return uwRet;
}

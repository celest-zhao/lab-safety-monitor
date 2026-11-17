/* view_task.c：显示任务（LCD 已交由 sensor_task，本任务仅保留 Scene 握手与错误显示） */
#include <stdio.h>
#include <stdlib.h>
#include "string.h"
#include "lcd.h"
#include "gui.h"
#include "stdio.h"
#include "usart_api.h"
#include "rtc_api.h"
#include "task_inter.h"
#include "key.h"
#include "view_task.h"
#include "error.h"

/******************************内部参数宏定义***********************************/
#define   PRINTF_COLOR     BLACK    //打印输出颜色
#define   PRINTF_Y         150      //打印数据Y轴位置
#define   PRINTF_X         0        //打印数据X轴位置

UINT32 g_ViewTskHandle;

/******************************内部变量定义************************************/
uint16_t ColorTab[5] = {RED, GREEN, BLUE, YELLOW, BRED}; //定义颜色数组
char  Str_Code[20];   //显示屏显示缓冲数组

Key_Struct key1 = {1};
Key_Struct key2 = {2};
Key_Struct key3 = {3};
Key_Struct key4 = {4};

/******************************内部函数声明************************************/


/* 按键扫描处理 */
void keytask(void)
{
    get_key_value(&key1, Key1_Read_Value);
    Key_Handle(&key1);
    get_key_value(&key2, Key2_Read_Value);
    Key_Handle(&key2);
    get_key_value(&key3, Key3_Read_Value);
    Key_Handle(&key3);
    get_key_value(&key4, Key4_Read_Value);
    Key_Handle(&key4);
}


/* 显示任务初始化 */
void task_view_init(void)
{
    /* 开机展示页（“环境监测实训”）已删除：LCD 交由 sensor_task 初始化，
       开机直接显示综合概览页 */
    KEY_Init();
    Scene.state = 1; //初始化状态为1
}

/* 显示任务：保持 Scene 握手 + 错误信息显示 */
VOID view_task(VOID)
{
    UINT32 uwRet = LOS_OK;
    while(1)
    {
        /* 保持 Scene 握手应答 */
        if (Scene.state == 1) { Scene.req_state = 1; }
        else                  { Scene.req_state = 0; }
		Error_Msg_View();
        //keytask();  /* 按键已交给 sensor_task */
        uwRet = LOS_TaskDelay(10);
        if(uwRet != LOS_OK)
        {
            return;
        }
    }
}

/* 创建显示任务 */

UINT32 creat_view_task(VOID)
{
    UINT32 uwRet = LOS_OK;
    TSK_INIT_PARAM_S task_init_param = {0};   /* 未使用字段必须清零 */

    task_init_param.usTaskPrio = 1;
    task_init_param.pcName = "view_task";
    task_init_param.pfnTaskEntry = (TSK_ENTRY_FUNC)view_task;
    task_init_param.uwStackSize = 0xA00;
    uwRet = LOS_TaskCreate(&g_ViewTskHandle, &task_init_param);
    if(LOS_OK != uwRet)
    {
        return uwRet;
    }
    return uwRet;
}



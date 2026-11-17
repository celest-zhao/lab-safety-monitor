#ifndef __task_inter_H
#define __task_inter_H

#include "stdint.h"
#include "ocprotocol.h"

#define NET_TYPE NET_TYPE_WIFI

void task_view_init(void);
void task_control_init(void);

void uart_taskhandle(void);
void control_taskhandle(void);
void task_view(void);

/* 界面/业务共享结构体（V1.0 按“智能门禁平台 家居场景”字段重排）
 * 说明：LCD 由 sensor_task 独占，本结构体只用于网络状态行与平台状态缓存，
 *       不参与屏幕刷新，避免 view_task 与 sensor_task 抢屏。 */
typedef struct
{
    uint8_t state;
    uint8_t res[3];
    uint8_t ID[20];
    uint8_t Line1[20];
    uint8_t Line2[20];
    uint8_t Line3[20];
    uint8_t Humidity[5];         /* "20.0~95.0" */
    uint8_t res2[3];
    uint8_t Temp[5];             /* "0.0~50.0"  */
    uint8_t res3[3];
    uint8_t GasInput;            /* 烟雾监测   */
    uint8_t BodyInput;           /* 运动检测   */
    uint8_t DoorInput;           /* 门禁状态   */
    uint8_t RelayOutput;         /* 水电控制   */
    uint8_t LEDOutput;           /* 家居照明   */
    uint8_t FanOutput;           /* 风扇状态   */
    uint8_t clr_screen_status;
    uint8_t res4[2];
} ViewDataStruct;

/*外部可调用结构体*/
extern ViewDataStruct HomeView;

#endif

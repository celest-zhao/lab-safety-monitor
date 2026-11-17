#ifndef __SENSOR_TASK_H
#define __SENSOR_TASK_H
#ifdef __cplusplus
extern "C" {
#endif

/* Includes LiteOS (mirror control_task.h / view_task.h) ------------------------*/
#include "los_base.h"
#include "los_config.h"
#include "los_sys.h"
#include "los_typedef.h"
#include "los_task.ph"
#include "los_hwi.h"
#include "los_sem.h"
#include "los_event.h"
#include "los_memory.h"
#include "los_queue.ph"

#include "ocprotocol.h"     /* PTL_DATA_STRUCT（平台上报结构体） */


/* 平台可控开关编号 */
#define PLAT_SW_FAN     0u      /* 风扇状态 -> PA8  */
#define PLAT_SW_LAMP    1u      /* 居家照明 -> PB15 */

UINT32 creat_sensor_task(VOID);
void   task_sensor_init(void);

/* 平台对接接口 */
void    Plat_FillReport(PTL_DATA_STRUCT *dst);
void    Plat_SetSwitch(uint8_t which, uint8_t on);
uint8_t Plat_GetFan(void);
uint8_t Plat_GetLamp(void);
uint8_t Plat_IsManual(void);


float   Plat_GetTemp(void);     /* 最近一次采样温度，单位 ℃     */
float   Plat_GetHumi(void);     /* 最近一次采样湿度，单位 %RH    */

/* [华为云·按物模型补齐 8 属性] 扩展取值接口 */
uint8_t  Plat_GetGas(void);     /* 1=检测到气体 0=正常          */
uint8_t  Plat_GetBody(void);    /* 1=有人 0=无人                */
uint16_t Plat_GetLight(void);   /* 光照，单位 lx                */
uint8_t  Plat_GetAiSim(void);   /* AI similarity 0~100 (100=normal) */
float    Plat_GetTempRise(void); /* 10s temp rise (C) */
uint8_t  Plat_GetSoundOn(void); /* 报警声音状态：1=开启（未静音） 0=已静音 */

/* 融合研判结论取值接口（供云端大模型对齐边缘判定结果）*/
uint8_t  Plat_GetFuseLevel(void);  /*Plat_GetFuseLevel()  综合风险等级*/
float    Plat_GetFuseScore(void);  /*Plat_GetFuseScore()  综合风险分数*/
uint8_t  Plat_GetDominant(void);  /*Plat_GetDominant()*/
/* [华为云·按物模型补齐 8 属性] 结束 --------------------------------------- */

#ifdef __cplusplus
}
#endif
#endif /* __SENSOR_TASK_H */

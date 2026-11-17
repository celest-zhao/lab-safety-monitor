/* main.c：程序入口，初始化硬件与各任务后启动 LiteOS */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "task_inter.h"
#include "sysclk.h"
/* Private function prototypes -----------------------------------------------*/
#include "control_task.h"
#include "view_task.h"
#include "sensor_task.h"   /* [ADD] 4-sensor fusion task */
/* 主函数 */
int main(void)
{
    UINT32 uwRet = LOS_OK;
    /*初始化*/
    HAL_Init();                    //硬件初始化
    sysclkinit();                  //系统时钟初始化
    task_view_init();              //显示任务初始化（不再画开机引导页）

    task_sensor_init();       /* [ADD] LCD + sensors + fusion + actuators */
    task_control_init();           //控制任务初始化
    uwRet = LOS_KernelInit();      //操作系统初始化

    if (uwRet != LOS_OK)
    {
        return LOS_NOK;
    }
    uwRet = creat_control_task();    //创建控制任务
    if (uwRet != LOS_OK)
    {
        return LOS_NOK;
    }
    uwRet = creat_view_task();       //创建显示任务
    if (uwRet != LOS_OK)
    {
        return LOS_NOK;
    }
        
    if (uwRet != LOS_OK)
    {
        return LOS_NOK;
    }
    uwRet = creat_sensor_task();  /* [ADD] 4-sensor fusion task */
    if (uwRet != LOS_OK)
    {
        return LOS_NOK;
    }
    LOS_Start();	     //启动操作系统
    while (1)
    {

    }
}
/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/

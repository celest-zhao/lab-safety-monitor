/* touch_key.c：触摸按键驱动 */

#include "touch_key.h"

/*****************************IO引脚宏定义**************************************/	
#define Touch_Key_Pin        GPIO_PIN_14
#define Touch_Key_GPIO_Port  GPIOD

/*****************************IO操作函数****************************************/	
#define	Touch_Key_STATE()     HAL_GPIO_ReadPin(Touch_Key_GPIO_Port, Touch_Key_Pin)     

/* Touch_Key_Init：初始化Touch_Key和蜂鸣器引脚 */
void Touch_Key_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    __HAL_RCC_GPIOD_CLK_ENABLE();         //使能GPIOD的工作时钟

    /* 配置PD14引脚 */
    GPIO_InitStruct.Pin = Touch_Key_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;//GPIO_PULLDOWN;
    HAL_GPIO_Init(Touch_Key_GPIO_Port, &GPIO_InitStruct);
}

/* Touch_Key_State_Handle：读取Touch_Key的状态 */
uint8_t Touch_Key_State(void)
{
    return Touch_Key_STATE();
}

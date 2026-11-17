/* as312.c：人体红外传感器驱动 */
#include "as312.h"

/*****************************IO引脚宏定义**************************************/	
#define AS312_Pin            GPIO_PIN_8
#define AS312_GPIO_Port      GPIOC

/*****************************IO操作函数****************************************/	
#define	AS312_STATE()        HAL_GPIO_ReadPin(AS312_GPIO_Port, AS312_Pin)     

/* AS312_Init：初始化AS312 */
void AS312_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    __HAL_RCC_GPIOC_CLK_ENABLE();         //使能GPIOC的工作时钟

    /* 配置PC8引脚 */
    GPIO_InitStruct.Pin = AS312_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(AS312_GPIO_Port, &GPIO_InitStruct);
}

/* Infrared_Read_State：读取AS312的状态 */
uint8_t Infrared_Read_State(void)
{
    return AS312_STATE();
}

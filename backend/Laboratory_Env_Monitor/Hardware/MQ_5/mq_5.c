/* mq_5.c：气体传感器驱动 */

#include "mq_5.h"

/*****************************IO引脚宏定义**************************************/	
#define MQ5_Pin GPIO_PIN_15
#define MQ5_GPIO_Port GPIOD

/*****************************IO操作函数****************************************/	
#define	MQ5_STATE()     HAL_GPIO_ReadPin(MQ5_GPIO_Port, MQ5_Pin)     

/* MQ5_Init：初始化MQ5 */
void MQ5_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    __HAL_RCC_GPIOD_CLK_ENABLE();         //使能GPIOD的工作时钟

    /* 配置PB15引脚 */
    GPIO_InitStruct.Pin = MQ5_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(MQ5_GPIO_Port, &GPIO_InitStruct);
}

/* MQ5_Read_State：读取MQ5的状态 */
uint8_t MQ5_Read_State(void)
{
    return MQ5_STATE();
}

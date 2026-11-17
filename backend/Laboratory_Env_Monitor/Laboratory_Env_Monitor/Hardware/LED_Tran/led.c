/*******************************************************************************
* @文件名        : led
* @描述          : LED配置相关函数
* @作者          : 物联网项目组
* @版本          : V2.0.0
*******************************************************************************/

#include "LED.h"

/*****************************IO引脚宏定义**************************************/	
#define LED_Pin GPIO_PIN_15           //LED对应的引脚
#define LED_GPIO_Port GPIOB           //LED对应的端口

/*****************************IO操作函数****************************************/	
#define LED_Out_High()      HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET)      //打开LED
#define LED_Out_Low()       HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET)    //关闭LED

/*******************************************************************************
* 名    称：LED_Init
* 功    能：初始化LED引脚
* 参数说明：无
* 返 回 值：无
* 说    明：配置LED相应引脚
* 调用方法：外部函数
*******************************************************************************/
void LED_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;

    __HAL_RCC_GPIOB_CLK_ENABLE();         //使能GPIOB的工作时钟

    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);     

    /* 配置PB2引脚 */
    GPIO_InitStruct.Pin = LED_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);
}

/*******************************************************************************
* 名    称：LED_Handle
* 功    能：控制LED开关函数
* 参数说明：date: LED_OPEN  打开LED
                  LED_OFF   关闭LED
* 返 回 值：无
* 说    明：控制LED开关
* 调用方法：外部函数
*******************************************************************************/
void LED_Handle(uint8_t date)
{
    if(date == LED_OPEN) 
        LED_Out_High();
    else if(date == LED_OFF)
        LED_Out_Low();
}


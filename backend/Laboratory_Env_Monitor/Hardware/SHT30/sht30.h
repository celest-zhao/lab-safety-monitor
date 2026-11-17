#ifndef SHT30_H
#define SHT30_H

#include "stdio.h"
#include "string.h"
#include "stm32l4xx_hal.h"


void SHT30_read_result(float *Temperature,float *Humidity);

void SHT30_GPIO_Init(void);

void SysTick_Handler(void);
void delay_init(uint8_t SYSCLK);
void delay_xms(uint32_t nms);
void delay_s(uint32_t ns);




#endif



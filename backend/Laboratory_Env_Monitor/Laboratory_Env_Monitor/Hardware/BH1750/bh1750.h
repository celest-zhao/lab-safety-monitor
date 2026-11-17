/* bh1750.h：BH1750 光强传感器驱动接口 */

#ifndef __BH1750_H__
#define __BH1750_H__

#include "stm32l4xx_hal.h"

//******************************外部函数定义************************************/
extern void Init_BH1750(void);
extern uint16_t BH1750_Handle(void);

#endif



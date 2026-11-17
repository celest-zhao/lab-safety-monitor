#ifndef __LD3320_H
#define	__LD3320_H

#include "stm32l4xx.h"
 
/* 以下三个状态定义用来记录程序是在运行ASR识别还是在运行MP3播放 */
#define LD_MODE_IDLE		0x00
#define LD_MODE_ASR_RUN		0x08
#define LD_MODE_MP3		 	0x40

/* 以下五个状态定义用来记录程序是在运行ASR识别过程中的哪个状态 */
#define LD_ASR_NONE				0x00	//	表示没有在作ASR识别
#define LD_ASR_RUNING			0x01	//	表示LD3320正在作ASR识别中
#define LD_ASR_FOUNDOK			0x10	//	表示一次识别流程结束后，有一个识别结果
#define LD_ASR_FOUNDZERO 		0x11	//	表示一次识别流程结束后，没有识别结果
#define LD_ASR_ERROR	 		0x31	//	表示一次识别流程中LD3320芯片内部出现不正确的状态

/*
识别码定义，需要用户进行修改
*/
#define DATE_A               4   // 自定义的识别语音条数
#define DATE_B               40	 // 自定义关键字存放数组大小


#define CODE_OPEN_LED	     1	 // 开灯
#define CODE_CLOSE_LED	     2	 // 关灯
#define CODE_TURN_ON	     3	 // 断开开关即控制继电器断开输出
#define CODE_TURN_OFF	     4	 // 打开开关即控制继电器输出

#define CODE_LSD	1	 /*流水灯*/
/* 定义LD3320数据结构体 */
typedef struct 
{
	uint8_t LD_Mode;        /* 用来记录当前是在进行ASR识别还是在播放MP3 */
	uint8_t RegVal;         /* 存放寄存器的值 */
	uint8_t AsrStatus;	    /* ASR运行状态 */
	uint8_t AsrRes;         /* ASR运行结果返回值 */
	uint8_t int_flag;       /* LD3320 发生中断标志*/
	
	uint8_t DemoFlag;       /* 播放demo mp3 标志位*/
	uint32_t Mp3StartPos;   /* mp3音频数组起始指针 */
	uint32_t Mp3Size;       /* mp3音频数组大小 */
	uint32_t Mp3Pos;        /* mp3音频数组指针 */
	uint32_t CurMp3Pos;     /* 当前mp3音频数组指针 */
	uint8_t  Mp3PlayState;	/* 用来记录播放MP3的状态 */
	uint8_t HighInt;        /* LD3320 中断允许 */
	uint8_t LowInt;         /* LD3320 FIFO中断允许 */
	uint8_t FIFOStatus;     /* FIFO状态 */
	uint8_t SPVol;          /* 喇叭输出的音量 */
	uint8_t play_num;       /* 播放音频的序号 */
}LD3320_TYPE;



/* 外部可调用函数声明 */
extern void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi);
extern void EXTI9_5_IRQHandler(void);
extern void LD3320_Init(void);
extern void LD3320_start(void);
extern void ProcessInt0(void);
extern void PlayDemoSound_Once(void);
extern void UserCodeHandle(void);

/* 外部可调用参数声明 */
extern LD3320_TYPE ld3320_Struct;
/* 使用外部定义的函数 */
extern uint8_t  sRecog[DATE_A][DATE_B];
extern uint8_t  pCode[DATE_A];
#endif /*__LD3320_H */


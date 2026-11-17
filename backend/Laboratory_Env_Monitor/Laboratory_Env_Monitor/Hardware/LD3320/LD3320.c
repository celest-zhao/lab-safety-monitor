/* LD3320.c：语音识别模块驱动 */
#include "LD3320.h"
#include "DemoSound1.h"   /* [MOD] smoke-alarm sound */
#include "string.h"
#include "spi_api.h"

#define CLK_IN   	         24
#define LD_PLL_11			(uint8_t)((CLK_IN/2.0)-1)
#define LD_PLL_MP3_19		0x0f
#define LD_PLL_MP3_1B		0x18
#define LD_PLL_MP3_1D   	(uint8_t)(((90.0*((LD_PLL_11)+1))/(CLK_IN))-1)

#define LD_PLL_ASR_19 		(uint8_t)(CLK_IN*32.0/(LD_PLL_11+1) - 0.51)
#define LD_PLL_ASR_1B 		0x48
#define LD_PLL_ASR_1D 		0x1f


#define        RESUM_OF_MUSIC               0x01
#define        CAUSE_MP3_SONG_END           0x20

#define        MASK_INT_SYNC				0x10
#define        MASK_INT_FIFO				0x04
#define    	   MASK_AFIFO_INT				0x01
#define        MASK_FIFO_STATUS_AFULL		0x08


#define        MIC_VOL                      0x43 

#define        LD_SUCCESS              0x01
#define        LD_FAIL                 0x02

#define        LD_ASR_BUSY         0x01
#define        LD_ASR_IDLE         0x02

#define LD_DELAY_MS(x)  HAL_Delay(x)


#define LD_CS_Pin                   GPIO_PIN_0
#define LD_CS_GPIO_Port             GPIOD
#define LD_CS_Port_Clk_Enable       __HAL_RCC_GPIOD_CLK_ENABLE();

#define LD_RST_Pin                   GPIO_PIN_10
#define LD_RST_GPIO_Port             GPIOD
#define LD_RST_Port_Clk_Enable       __HAL_RCC_GPIOD_CLK_ENABLE();

#define LD_WR_Pin                   GPIO_PIN_12
#define LD_WR_GPIO_Port             GPIOA
#define LD_WR_Port_Clk_Enable       __HAL_RCC_GPIOA_CLK_ENABLE();

#define LD_IRQ_Pin                   GPIO_PIN_12
#define LD_IRQ_GPIO_Port             GPIOB
#define LD_IRQ_Clk_Enable           __HAL_RCC_GPIOB_CLK_ENABLE();
#define LD_EXIT_IRQn                EXTI15_10_IRQn
#define LD_EXIT_IRQnHandler         EXTI15_10_IRQHandler
#define LD_RST_H()   HAL_GPIO_WritePin(LD_RST_GPIO_Port ,LD_RST_Pin , GPIO_PIN_SET);
#define LD_RST_L()   HAL_GPIO_WritePin(LD_RST_GPIO_Port ,LD_RST_Pin , GPIO_PIN_RESET);

#define LD_CS_H()	 HAL_GPIO_WritePin(LD_CS_GPIO_Port ,LD_CS_Pin , GPIO_PIN_SET);
#define LD_CS_L()	 HAL_GPIO_WritePin(LD_CS_GPIO_Port ,LD_CS_Pin , GPIO_PIN_RESET);

#define LD_SPIS_H()  HAL_GPIO_WritePin(LD_WR_GPIO_Port ,LD_WR_Pin , GPIO_PIN_SET);
#define LD_SPIS_L()  HAL_GPIO_WritePin(LD_WR_GPIO_Port ,LD_WR_Pin , GPIO_PIN_RESET);


LD3320_TYPE ld3320_Struct = {
        .LD_Mode = LD_MODE_IDLE,	
        .RegVal=0,
        .play_num=0,
        .Mp3StartPos=0,
        .Mp3Size=0,
        .Mp3Pos=0,
        .CurMp3Pos=0,
        .Mp3PlayState=0,						
        .SPVol=15
};
SPI_HandleTypeDef SPI3_Handler;
#define LD_SPI_GPIO_Init  	spi_open(spi3,SPI_BAUDRATEPRESCALER_64,Falling)
#define SPI3_ReadWriteByte(data)   spi_ReadWriteByte(spi3,data)
static void SPI3_Init(void);
static void EXTI_Init(void);
static void LD_WriteReg(uint8_t reg_addr,uint8_t data);
static uint8_t LD_ReadReg(uint8_t reg_addr);
static void LD_reset(void);
static void LD_Init_Common(void);
static void LD_Init_MP3(void);
static void LD_play(void);
static void LD_AdjustMIX2SPVolume(uint8_t val);
static void LD_ReloadMp3Data(void);
static void LD_ReloadMp3Data_Again(void);

/* SPI3_Init：SPI3 初始化 */
void SPI3_Init(void)
{
    LD_SPI_GPIO_Init;
    SPI3_ReadWriteByte(0Xff);
}
/* EXTI_Init：中断引脚初始化 */
void EXTI_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    LD_IRQ_Clk_Enable;

    GPIO_InitStruct.Pin = LD_IRQ_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(LD_IRQ_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  /* 中断优先级降到 5，避免阻塞 sensor_task */
  HAL_NVIC_SetPriority(LD_EXIT_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(LD_EXIT_IRQn);  
}
/* LD_EXIT_IRQnHandler：语音模块中断服务 */
void LD_EXIT_IRQnHandler(void)
{
    if(__HAL_GPIO_EXTI_GET_IT(LD_IRQ_Pin) != RESET)
    {
        ld3320_Struct.int_flag=1;
        /* ProcessInt0 移出中断，改到任务上下文执行 */
        __HAL_GPIO_EXTI_CLEAR_IT(LD_IRQ_Pin);
    }
}
/* LD3320_Init：LD3320 语音模块初始化 */
void LD3320_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    
    LD_RST_Port_Clk_Enable;
    LD_CS_Port_Clk_Enable;
    LD_WR_Port_Clk_Enable;
    
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;   
    
    GPIO_InitStruct.Pin = LD_RST_Pin;
    HAL_GPIO_Init(LD_RST_GPIO_Port, &GPIO_InitStruct);
    
    GPIO_InitStruct.Pin = LD_WR_Pin;
    HAL_GPIO_Init(LD_WR_GPIO_Port, &GPIO_InitStruct); 

    GPIO_InitStruct.Pin = LD_CS_Pin;	
    HAL_GPIO_Init(LD_CS_GPIO_Port, &GPIO_InitStruct);	

    LD_CS_H();
    SPI3_Init();	
    EXTI_Init();	
    LD_reset();

    ld3320_Struct.Mp3PlayState = 0;

}
__weak void UserCodeHandle(void)
{
  
}

/* LD_WriteReg：写 LD3320 寄存器 */
void LD_WriteReg(uint8_t reg_addr,uint8_t data)
{
	LD_CS_L();

	SPI3_ReadWriteByte(0x04);

	SPI3_ReadWriteByte(reg_addr);

	SPI3_ReadWriteByte(data);
	
	LD_CS_H();
}
/* LD_ReadReg：读 LD3320 寄存器 */
uint8_t LD_ReadReg(uint8_t reg_addr)
{
	uint8_t read_data= 0;
	
	LD_CS_L();

	SPI3_ReadWriteByte(0x05);

	SPI3_ReadWriteByte(reg_addr);
	
	read_data = SPI3_ReadWriteByte(0x00);			

	LD_CS_H();
		
	return(read_data);
}
/* LD_reset：LD3320 复位 */
void LD_reset(void)
{
	LD_RST_H();
	LD_DELAY_MS(100);
	LD_RST_L();
	LD_DELAY_MS(100);
	LD_RST_H();
	
	LD_DELAY_MS(100);
	LD_CS_L();
	LD_DELAY_MS(100);
	LD_CS_H();		
	LD_DELAY_MS(100);
	LD_SPIS_L();
}

/* LD_Init_Common：公共寄存器初始化 */
void LD_Init_Common(void)
{
	LD_ReadReg(0x06);  
	LD_WriteReg(0x17, 0x35); 
	LD_DELAY_MS(5);
	LD_ReadReg(0x06);  

	LD_WriteReg(0x89, 0x03);  
	LD_DELAY_MS(5);
	LD_WriteReg(0xCF, 0x43);   
	LD_DELAY_MS(5);
	LD_WriteReg(0xCB, 0x02);
	
	/*PLL setting*/
	LD_WriteReg(0x11, LD_PLL_11);       
	if (ld3320_Struct.LD_Mode == LD_MODE_MP3)
	{
		LD_WriteReg(0x1E, 0x00); 
		LD_WriteReg(0x19, LD_PLL_MP3_19);   
		LD_WriteReg(0x1B, LD_PLL_MP3_1B);   
		LD_WriteReg(0x1D, LD_PLL_MP3_1D);
	}
	else
	{
		LD_WriteReg(0x1E,0x00);
		LD_WriteReg(0x19, LD_PLL_ASR_19); 
		LD_WriteReg(0x1B, LD_PLL_ASR_1B);		
	    LD_WriteReg(0x1D, LD_PLL_ASR_1D);
	}
	LD_DELAY_MS(5);
	
	LD_WriteReg(0xCD, 0x04);
	LD_WriteReg(0x17, 0x4c); 
	LD_DELAY_MS(1);
	LD_WriteReg(0xB9, 0x00);
	LD_WriteReg(0xCF, 0x4F); 
	LD_WriteReg(0x6F, 0xFF); 
}

/* ProcessInt0：中断数据处理（MP3 FIFO 服务） */
void ProcessInt0(void)
{

	
	ld3320_Struct.RegVal = LD_ReadReg(0x2B);

	
    ld3320_Struct.HighInt = LD_ReadReg(0x29); 
    LD_WriteReg(0x29,0) ;
    ld3320_Struct.LowInt = LD_ReadReg(0x02);
    LD_WriteReg(0x02,0) ;
    
    
    if(ld3320_Struct.LD_Mode == LD_MODE_MP3)  
    {  

                
        if(LD_ReadReg(0xBA)&CAUSE_MP3_SONG_END) 
        {
            LD_WriteReg(0x2B,  0);
            LD_WriteReg(0xBA, 0);	
            LD_WriteReg(0xBC,0x0);	
            LD_WriteReg(0x08,1);
            LD_WriteReg(0x08,0);
            LD_WriteReg(0x33, 0);
            ld3320_Struct.Mp3PlayState = 0;		//            
            return ;
        }
        
        if(ld3320_Struct.Mp3Pos>=ld3320_Struct.Mp3Size)
        {

            LD_WriteReg(0xBC, 0x01);
            LD_WriteReg(0x29, 0x10);
            ld3320_Struct.Mp3PlayState = 0;		// 
            return;	
        }
        	
        LD_ReloadMp3Data_Again();

        LD_WriteReg(0x29,ld3320_Struct.HighInt); 
        LD_WriteReg(0x02,ld3320_Struct.LowInt) ;
    }
}



/* LD_Init_MP3：MP3 播放初始化 */
void LD_Init_MP3(void)
{
	ld3320_Struct.LD_Mode = LD_MODE_MP3;
	LD_Init_Common();

	LD_WriteReg(0xBD,0x02);
	LD_WriteReg(0x17, 0x48);
	LD_DELAY_MS(1);

	LD_WriteReg(0x85, 0x52); 
	LD_WriteReg(0x8F, 0x00);  
	LD_WriteReg(0x81, 0xff);   //
	LD_WriteReg(0x83, 0xff);   //
	LD_WriteReg(0x8E, 0xff);
	LD_WriteReg(0x8D, 0xff);
//    delay_us(1);
	LD_WriteReg(0x87, 0xff);
	LD_WriteReg(0x89, 0xff);
//	delay_us(1);
	LD_WriteReg(0x22, 0x00);    
	LD_WriteReg(0x23, 0x00);
	LD_WriteReg(0x20, 0xef);    
	LD_WriteReg(0x21, 0x07);
	LD_WriteReg(0x24, 0x77);          
	LD_WriteReg(0x25, 0x03);
	LD_WriteReg(0x26, 0xbb);    
	LD_WriteReg(0x27, 0x01);

}
/* LD_play：播放音频 */
void LD_play(void)
{
	ld3320_Struct.Mp3Pos=0;
	ld3320_Struct.Mp3PlayState=1;

	if (ld3320_Struct.Mp3Pos >=  ld3320_Struct.Mp3Size)
		return ; 

	LD_ReloadMp3Data();
//	LD_Fill_FIFO();
    LD_WriteReg(0xBA, 0x00);
	LD_WriteReg(0x17, 0x48);
	LD_WriteReg(0x33, 0x01);
	LD_WriteReg(0x29, 0x04);
	
	LD_WriteReg(0x02, 0x01);
	LD_WriteReg(0x85, 0x5A);


}
/* LD_AdjustMIX2SPVolume：调节喇叭音量 */
void LD_AdjustMIX2SPVolume(uint8_t val)
{
    if(val >= 15)
        val = 15;
	ld3320_Struct.SPVol = val;
	val = ((15-val)&0x0f) << 2;
	LD_WriteReg(0x8e, val | 0xc3);
	LD_WriteReg(0x87, 0x78);
}
//void LD_Fill_FIFO(void)
//{
//	uint8_t status;
//	status = LD_ReadReg(0x06);
//    
//	while (!(status&MASK_FIFO_STATUS_AFULL))
//	{
//		LD_WriteReg(0x01,0xff);
//		status = LD_ReadReg(0x06);
//	}
//}
/* LD_ReloadMp3Data：加载 MP3 数据到 FIFO */
void LD_ReloadMp3Data(void)
{
	uint8_t val;
//	uint8_t k;

	if (ld3320_Struct.DemoFlag)
	{
		ld3320_Struct.FIFOStatus = LD_ReadReg(0x06);
		while ( !(ld3320_Struct.FIFOStatus&MASK_FIFO_STATUS_AFULL) && (ld3320_Struct.Mp3Pos<ld3320_Struct.Mp3Size) )
		{
			if(ld3320_Struct.play_num==1)
				val = bpDemoSound1[ld3320_Struct.Mp3Pos++];
			else if(ld3320_Struct.play_num==2)
				val = bpDemoSound2[ld3320_Struct.Mp3Pos++];
			else if(ld3320_Struct.play_num==3)
				val = bpDemoSound3[ld3320_Struct.Mp3Pos++];
				
			LD_WriteReg(0x01,val);
			ld3320_Struct.FIFOStatus = LD_ReadReg(0x06);
		}		
	}
}
/* LD_ReloadMp3Data_Again：再次加载 MP3 数据 */
void LD_ReloadMp3Data_Again(void)
{
//	uint32_t nCurMp3Pos = 0;
	uint8_t val;

//	nCurMp3Pos = ld3320_Struct.Mp3StartPos + ld3320_Struct.Mp3Pos;

	ld3320_Struct.FIFOStatus = LD_ReadReg(0x06);
	while ( !(ld3320_Struct.FIFOStatus&MASK_FIFO_STATUS_AFULL) && (ld3320_Struct.Mp3Pos<ld3320_Struct.Mp3Size) )
	{
		if(ld3320_Struct.play_num==1)
				val = bpDemoSound1[ld3320_Struct.Mp3Pos++];
			else if(ld3320_Struct.play_num==2)
				val = bpDemoSound2[ld3320_Struct.Mp3Pos++];
			else if(ld3320_Struct.play_num==3)
				val = bpDemoSound3[ld3320_Struct.Mp3Pos++];

		LD_WriteReg(0x01,val);
		ld3320_Struct.FIFOStatus = LD_ReadReg(0x06);
	}
	
	/* 播放完直接返回，结束由下次中断处理，全程非阻塞 */

}
/* PlayDemoSound_Once：播放一次提示音 */
void PlayDemoSound_Once(void)
{
	ld3320_Struct.DemoFlag = 1;
	ld3320_Struct.Mp3StartPos = 0;
	
	if(ld3320_Struct.play_num==1)
    {
        ld3320_Struct.Mp3Size = sizeof(bpDemoSound1);
    }
	else if(ld3320_Struct.play_num==2)
    {
        ld3320_Struct.Mp3Size = sizeof(bpDemoSound2);
    }
	else if(ld3320_Struct.play_num==3)
    {
        ld3320_Struct.Mp3Size = sizeof(bpDemoSound3);
    }

    
	LD_Init_MP3();
	LD_AdjustMIX2SPVolume(10);
//	SPI3_SetSpeed(SPI_BAUDRATEPRESCALER_4);
	LD_play();
}




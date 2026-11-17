/* bh1750.c：BH1750 光强传感器驱动 */

#include "bh1750.h"

/********************************内部引脚定义***********************************/	   
#define BH1750_SCL_Pin              GPIO_PIN_13        
#define BH1750_SCL_GPIO_Port        GPIOB

#define BH1750_SDA_Pin              GPIO_PIN_14        
#define BH1750_SDA_GPIO_Port        GPIOB

/********************************IO模式设置*************************************/	
#define BH1750_SDA_IN()             {GPIOB->MODER&=~(3<<(14*2));GPIOB->MODER|=0<<14*2;}	      // PB14输入模式
#define BH1750_SDA_OUT()            {GPIOB->MODER&=~(3<<(14*2));GPIOB->MODER|=1<<14*2;}       // PB14输出模式

/********************************IO口函数操作***********************************/				   
#define BH1750_SCLK_Clr() 	HAL_GPIO_WritePin(BH1750_SCL_GPIO_Port, BH1750_SCL_Pin, GPIO_PIN_RESET) 
#define BH1750_SCLK_Set() 	HAL_GPIO_WritePin(BH1750_SCL_GPIO_Port, BH1750_SCL_Pin, GPIO_PIN_SET) 

#define BH1750_SDA_Clr()    HAL_GPIO_WritePin(BH1750_SDA_GPIO_Port, BH1750_SDA_Pin, GPIO_PIN_RESET)   
#define BH1750_SDA_Set()    HAL_GPIO_WritePin(BH1750_SDA_GPIO_Port, BH1750_SDA_Pin, GPIO_PIN_SET)

#define BH1750_READ_SDA()   HAL_GPIO_ReadPin(BH1750_SDA_GPIO_Port, BH1750_SDA_Pin)

/********************************BH1750宏定义***********************************/
#define BH1750_ADD_WRITE                 0x46   // 写数据
#define BH1750_ADD_READ                  0x47   // 读数据
#define BH1750_RSET                      0x07   // 复位
#define BH1750_POWER_ON                  0x01   // 打开模块
#define BH1750_POWER_DOWN                0x00   // 关闭模块
#define BH1750_MODE_H1                   0x10   // 高分辨率1 单位1lx
#define BH1750_MODE_H2                   0x11   // 高分辨率2 单位0.51lx
#define BH1750_MODE_L                    0x13   // 低分辨率 单位4lx

/********************************内部宏定义*************************************/
#define SYSCLOCK   80         // 系统时钟，用于延时,需要用户根据系统时钟进行修改，要不会影响精确延时，单位Mhz
#define ACK        1
#define NOACK      0
#define TURE       0  
#define ERROR      1

//******************************内部函数定义************************************/
static void delay_us(uint16_t Nus);
static void delay_ms(uint16_t Nms);
static void BH1750_GPIO_Init(void);
static void BH1750_IIC_Start(void);
static void BH1750_IIC_Stop(void);
static uint8_t BH1750_IIC_Wait_Ack(void);
static void BH1750_IIC_Ack(void);
static void BH1750_IIC_No_Ack(void);
static void BH1750_Write_Byte(uint8_t date);
static uint8_t BH1750_Read_Byte(uint8_t ack);
static void BH1750_Write_Command(uint8_t cmd);
static void Start_BH1750(void);
static uint16_t Read_BH1750(void);

/* delay_us：微秒级延时 */
static void delay_us(uint16_t Nus)
{
    uint32_t i;
    i = Nus * (SYSCLOCK/3);        
    while(i--);
}

/* delay_ms：微秒级延时 */
static void delay_ms(uint16_t Nms)
{
    uint32_t i;
    i = Nms * (SYSCLOCK/3) * 1000;        
    while(i--);
}

/* BH1750_GPIO_Init：初始化BH1750引脚 */
static void BH1750_GPIO_Init(void)
{					     
    GPIO_InitTypeDef GPIO_InitStruct;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitStruct.Pin = BH1750_SCL_Pin | BH1750_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BH1750_SCL_GPIO_Port, &GPIO_InitStruct);
    
    GPIO_InitStruct.Pin = GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
    

    HAL_GPIO_WritePin(GPIOD,GPIO_PIN_12,GPIO_PIN_SET);
   
}

/* BH1750_IIC_Start：产生IIC开始信号 */
static void BH1750_IIC_Start(void)
{
	BH1750_SDA_OUT();     
	BH1750_SDA_Set();
	BH1750_SCLK_Set();
    delay_us(5);
	BH1750_SDA_Clr();
    delay_us(5);
	BH1750_SCLK_Clr();
}	  

/* BH1750_IIC_Start：产生IIC停止信号 */
static void BH1750_IIC_Stop(void)
{
	BH1750_SDA_OUT();
	BH1750_SCLK_Clr();
	BH1750_SDA_Clr();
    delay_us(5);
	BH1750_SCLK_Set();  
	BH1750_SDA_Set();
    delay_us(5);    
}
/* BH1750_IIC_Wait_Ack：等待IIC应答信号 */
static uint8_t BH1750_IIC_Wait_Ack(void)
{
	uint8_t ack_time=0;
    
	BH1750_SDA_IN();       
	BH1750_SDA_Set();
    delay_us(2);	   
	BH1750_SCLK_Set();
    delay_us(2);	 
    
	while(BH1750_READ_SDA())
	{
		ack_time++;
		delay_us(10);
		if(ack_time>250)
		{
			BH1750_IIC_Stop();
			return ERROR;
		}
	}
	BH1750_SDA_Clr();//时钟输出0 	   
	return TURE;  
} 

/* BH1750_IIC_Ack：产生ACK应答 */
static void BH1750_IIC_Ack(void)
{
	BH1750_SCLK_Clr();
	BH1750_SDA_OUT();
	BH1750_SDA_Clr();
	delay_us(2);
	BH1750_SCLK_Set() ;
	delay_us(2);
	BH1750_SCLK_Clr();
	
}	
/* BH1750_IIC_No_Ack：不产生ACK应答 */
static void BH1750_IIC_No_Ack(void)
{
	BH1750_SCLK_Clr();
	BH1750_SDA_OUT();
	BH1750_SDA_Set();
	delay_us(2);
	BH1750_SCLK_Set();
	delay_us(2);
	BH1750_SCLK_Clr();
}					 				     
	
/* BH1750_Write_Byte：IIC发送一个数据 */
static void BH1750_Write_Byte(uint8_t date)
{                        
    uint8_t i;
    
	BH1750_SDA_OUT(); 
	BH1750_SCLK_Clr();
	for(i=0; i<8; i++)		
	{
		if((date&0x80) == 0x80)
		{
			BH1750_SDA_Set();
		}
		else
		{
			BH1750_SDA_Clr();
		}
		date = date << 1;
		delay_us(2);
		BH1750_SCLK_Set();
		delay_us(2);
		BH1750_SCLK_Clr();
		delay_us(2);
	}
} 	    

/* BH1750_Read_Byte：IIC发送一个数据 */
static uint8_t BH1750_Read_Byte(uint8_t ack)
{
	uint8_t i,receive = 0;
    
	BH1750_SDA_IN();
    for(i=0;i<8;i++ )
	{
        BH1750_SCLK_Clr(); 
        delay_us(2);
		BH1750_SCLK_Set();
        receive <<= 1;
        if(BH1750_READ_SDA())
            receive++;   
		delay_us(2); 
    }					 
    if (!ack)
        BH1750_IIC_No_Ack();
    else
        BH1750_IIC_Ack();  
    return receive;
}

/* BH1750_Write_Command：写指令到BH1750中 */
static void BH1750_Write_Command(uint8_t cmd)
{
    BH1750_IIC_Start();    
    BH1750_Write_Byte(BH1750_ADD_WRITE); 
    BH1750_IIC_Wait_Ack();    
    BH1750_Write_Byte(cmd);    
    BH1750_IIC_Wait_Ack();
    BH1750_IIC_Stop();                   
}

/* Init_BH1750：初始化BH1750 */
void Init_BH1750(void)
{
    BH1750_GPIO_Init();	
}

/* Start_BH1750：开始测量光的强度 */
static void Start_BH1750(void)
{
	BH1750_Write_Command(BH1750_POWER_ON);	 
	BH1750_Write_Command(BH1750_MODE_H1);
}

/* Read_BH1750：读取光强度 */
static uint16_t Read_BH1750(void)
{   
    uint16_t light_buf = 0;	
    
    BH1750_IIC_Start();                         
    BH1750_Write_Byte(BH1750_ADD_READ);         // 发送读数据信号
    BH1750_IIC_Wait_Ack();
    
    light_buf = BH1750_Read_Byte(ACK);          // 读数据的高八位
    light_buf <<= 8;
    light_buf |= BH1750_Read_Byte(NOACK);       // 读数据的低八位
    
    BH1750_IIC_Stop();       
    return  light_buf;
}

/* BH1750_Handle：读取光强度 */
uint16_t BH1750_Handle(void)
{   
    uint16_t result_lx = 0;

    Start_BH1750();
    delay_ms(180);
    result_lx = Read_BH1750();
    result_lx = (float)(result_lx / 1.2);

    return result_lx;
}


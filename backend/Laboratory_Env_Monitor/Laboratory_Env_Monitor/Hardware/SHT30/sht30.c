/* sht30.c：SHT30 温湿度传感器驱动 */
#include "sht30.h"

/********************************内部引脚定义***********************************/	   
#define SHT30_SCL_Pin              GPIO_PIN_13        
#define SHT30_SCL_GPIO_Port        GPIOB

#define SHT30_SDA_Pin              GPIO_PIN_14        
#define SHT30_SDA_GPIO_Port        GPIOB

/********************************IO模式设置*************************************/	
#define SHT30_SDA_IN()             {GPIOB->MODER&=~(3<<(14*2));GPIOB->MODER|=0<<14*2;}	      // PB14输入模式
#define SHT30_SDA_OUT()            {GPIOB->MODER&=~(3<<(14*2));GPIOB->MODER|=1<<14*2;}       // PB14输出模式

/********************************IO口函数操作***********************************/				   
#define SHT30_SCLK_Clr() 	HAL_GPIO_WritePin(SHT30_SCL_GPIO_Port, SHT30_SCL_Pin, GPIO_PIN_RESET) 
#define SHT30_SCLK_Set() 	HAL_GPIO_WritePin(SHT30_SCL_GPIO_Port, SHT30_SCL_Pin, GPIO_PIN_SET) 

#define SHT30_SDA_Clr()    HAL_GPIO_WritePin(SHT30_SDA_GPIO_Port, SHT30_SDA_Pin, GPIO_PIN_RESET)   
#define SHT30_SDA_Set()    HAL_GPIO_WritePin(SHT30_SDA_GPIO_Port, SHT30_SDA_Pin, GPIO_PIN_SET)

#define SHT30_READ_SDA()   HAL_GPIO_ReadPin(SHT30_SDA_GPIO_Port, SHT30_SDA_Pin)

/********************************SHT30宏定义***********************************/
//#define SHT30_ADD_WRITE                 0x46   // 写数据
//#define SHT30_ADD_READ                  0x47   // 读数据
//#define SHT30_RSET                      0x07   // 复位
//#define SHT30_POWER_ON                  0x01   // 打开模块
//#define SHT30_POWER_DOWN                0x00   // 关闭模块
//#define SHT30_MODE_H1                   0x10   // 高分辨率1 单位1lx
//#define SHT30_MODE_H2                   0x11   // 高分辨率2 单位0.51lx
//#define SHT30_MODE_L                    0x13   // 低分辨率 单位4lx

/********************************内部宏定义*************************************/
#define SYSCLOCK   80         // 系统时钟，用于延时,需要用户根据系统时钟进行修改，要不会影响精确延时，单位Mhz
#define ACK        1
#define NACK       0
#define TURE       0  
#define ERROR      1
#define write 0
#define read  1
//******************************内部函数定义************************************/
static void delay_us(uint16_t Nus);
static void delay_ms(uint16_t Nms);

static void SHT30_IIC_Start(void);
static void SHT30_IIC_Stop(void);
static uint8_t IIC_wait_ACK(void);
static void IIC_ACK(void);


/* delay_us：微秒级延时 */
static void delay_us(uint16_t Nus)
{
    uint32_t i;
    i = Nus * (SYSCLOCK/3);        
    while(i--);
}

/* delay_ms：毫秒级延时 */
static void delay_ms(uint16_t Nms)
{
    uint32_t i;
    i = Nms * (SYSCLOCK/3) * 1000;        
    while(i--);
}


/* SHT30_GOIO_Init：初始化SHT30引脚 */
void SHT30_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    __HAL_RCC_GPIOB_CLK_ENABLE();
	
    GPIO_InitStruct.Pin = SHT30_SCL_Pin | SHT30_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SHT30_SCL_GPIO_Port, &GPIO_InitStruct);
	
	/* 补光灯 PB15 配置与 SHT30 无关，已注释掉 */
#if 0
GPIO_InitStruct.Pin = GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    
    GPIO_InitStruct.Pin = GPIO_PIN_8;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_15,GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOD,GPIO_PIN_8,GPIO_PIN_RESET);
#endif   
	
}

/* IIC_ACK：产生ACK应答 */
static void IIC_ACK(void)
{
    SHT30_SCLK_Clr();
	SHT30_SDA_OUT();
	SHT30_SDA_Clr();
	delay_us(2);
	SHT30_SCLK_Set() ;
	delay_us(2);
	SHT30_SCLK_Clr();
}


/* IIC_wait_ACK：等待IIC应答信号 */
/*主机等待从机的ACK*/
static uint8_t IIC_wait_ACK(void)
{
    uint8_t ack_time=0;
    
	SHT30_SDA_IN();       
	SHT30_SDA_Set();
    delay_us(2);	   
	SHT30_SCLK_Set();
    delay_us(2);	 
    
	while(SHT30_READ_SDA())
	{
		ack_time++;
		delay_us(10);
		if(ack_time>250)
		{
			SHT30_IIC_Stop();
			return ERROR;
		}
	}
	SHT30_SDA_Clr();//时钟输出0 	   
	return TURE;  
}

/*功能： 启动IIC总线，即发送IIC起始条件*/
/* SHT30_IIC_Start：产生IIC开始信号 */
static void SHT30_IIC_Start(void)
{
    SHT30_SDA_OUT();     
	SHT30_SDA_Set();
	SHT30_SCLK_Set();
    delay_us(5);
	SHT30_SDA_Clr();
    delay_us(5);
	SHT30_SCLK_Clr();
}


/* SHT30_IIC_Stop：产生IIC停止信号 */
static void SHT30_IIC_Stop(void)
{
    SHT30_SDA_OUT();
	SHT30_SCLK_Clr();
	SHT30_SDA_Clr();
    delay_us(5);
	SHT30_SCLK_Set();  
	SHT30_SDA_Set();
    delay_us(5);  
}


void IIC_SendByte(uint8_t byte)
{
    uint8_t BitCnt;
    SHT30_SDA_OUT();
    SHT30_SCLK_Clr();
    for(BitCnt = 0; BitCnt < 8; BitCnt++)
    {
        if(byte & 0x80) { SHT30_SDA_Set(); }
        else { SHT30_SDA_Clr(); }
        byte <<= 1;
        delay_us(2);
        SHT30_SCLK_Set();
        delay_us(2);
        SHT30_SCLK_Clr();
        delay_us(2);
    }
}

uint8_t IIC_RcvByte(void)
{
    uint8_t BitCnt;
    uint8_t retc;
    retc = 0;
    SHT30_SDA_IN();
    delay_us(1);
    for(BitCnt = 0; BitCnt < 8; BitCnt++)
    {
        SHT30_SCLK_Clr();
        delay_us(2);
        SHT30_SCLK_Set();
        retc = retc << 1;
        if(SHT30_READ_SDA()) { retc |= 1; }
        delay_us(1);
    }
    SHT30_SCLK_Clr();
    return (retc);
}


/*温湿度获取函数
函数原型：SHT30_read_result(float *Temperature,float *Humidity)
功能：用来接收器件采集并合成温湿度
*/

void SHT30_read_result(float *Temperature,float *Humidity)
{
    uint16_t tem, hum;
    uint8_t buff[6] = {0};   /* 读失败时返回全 0，避免用到栈上残留值 */
	
    SHT30_IIC_Start();
    IIC_SendByte(0x44 << 1 | write);
    IIC_wait_ACK();
    IIC_SendByte(0x2C);
    IIC_wait_ACK();
    IIC_SendByte(0x06);
    IIC_wait_ACK();
    SHT30_IIC_Stop();
    delay_ms(50);
    SHT30_IIC_Start();
    IIC_SendByte(0x44 << 1 | read);
    if(IIC_wait_ACK() == 0)
    {
        buff[0] = IIC_RcvByte();
        IIC_ACK();
        buff[1] = IIC_RcvByte();
        IIC_ACK();
        buff[2] = IIC_RcvByte();
        IIC_ACK();
        buff[3] = IIC_RcvByte();
        IIC_ACK();
        buff[4] = IIC_RcvByte();
        IIC_ACK();
        buff[5] = IIC_RcvByte();
        IIC_ACK();
        SHT30_IIC_Stop();
    }

    tem = buff[0];
    tem = (tem << 8) + buff[1];
    hum = buff[3];
    hum = (hum << 8) + buff[4];
    //  tem = ((buff[0]<<8) | buff[1]);
    //  hum = ((buff[3]<<8) | buff[4]);
    //转换实际温度
    *Temperature =(175.0f * (float)tem / 65535.0f - 45.0f);
    *Humidity    =(100.0f * (float)hum / 65535.0f);
    //  return Temperature;
//	printf("############## Temperature:%d℃\r\n", tem);
//	printf("############## Humidity:%d\r\n\r\n", hum);
//	printf("**********温湿度：%.2lf°C  %.2lf%\r\n",*Temperature,*Humidity);
}



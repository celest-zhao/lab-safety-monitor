#ifndef ESP8285_H
#define ESP8285_H

#include "stdint.h"

#define SERVER_IP                   "d138d4a4fb.st1.iotda-device.cn-north-4.myhuaweicloud.com"
#define SERVER_PORT              	1883

#define WIFI_SSID						"Www"
#define WIFI_PSWD						"qdee6698"

#define WIFI_STA_MODE				"1"



#define PROD_ID                     "6adf09f67f2e6c302fa6f3b9_dev1"
#define USER_PSWD                   "12345678"
#define TIMESTAMP                   "2026102608"

#define CLIENT_ID            		(PROD_ID"_0_0_"TIMESTAMP)


#define MQTT_NO_RECONNECT			0	  
#define MQTT_RECONNECT				1	 

#define MQTT_QOS   					0
#define MQTT_SUB_TOPIC              ("$oc/devices/6adf09f67f2e6c302fa6f3b9_dev1/sys/messages/down")

/* [华为云命令下发] 命令下行主题。
     sys/messages/down  <- 平台“消息下发”（无 request_id，不需要回应）
     sys/commands/#     <- 平台“命令下发”（带 request_id，必须回 response）。 */
#define MQTT_SUB_TOPIC_CMD          ("$oc/devices/6adf09f67f2e6c302fa6f3b9_dev1/sys/commands/#")
#define MQTT_PUB_TOPIC              ("$oc/devices/6adf09f67f2e6c302fa6f3b9_dev1/sys/properties/report")

#define MQTT_PUB_RESPONSE_TOPIC     ("$oc/devices/6adf09f67f2e6c302fa6f3b9_dev1/sys/commands/response/request_id=")

#define HUAWEI_SERVICE_ID           "stm32"     /* 服务 ID（物模型里新建的服务）      */
#define HUAWEI_PROP_TEMP            "SHT30_T"   /* 温度，数据类型 decimal，支持 1 位小数 */
#define HUAWEI_PROP_HUMI            "SHT30_H"   /* 湿度，数据类型 decimal，支持 1 位小数 */


#define HUAWEI_PROP_GAS             "Gas"       /* 气体传感，string 类型                */
#define HUAWEI_PROP_BODY_IR         "Body_IR"   /* 人体监测，boolean 类型               */
#define HUAWEI_PROP_LIGHT           "Light"     /* 光强，decimal 类型（整数上报即可）     */
#define HUAWEI_PROP_SOUND           "Sound"     /* 声音，boolean（报警声音开：true；静音关：false）*/
#define HUAWEI_PROP_FILL            "Fill"      /* 补光灯，boolean（实际状态）          */
#define HUAWEI_PROP_FAN             "Fan"       /* 风扇，boolean（实际状态）            */
#define HUAWEI_PROP_AI_SCORE      "AI_Score"  /* AI similarity, decimal 0~100 */
#define HUAWEI_PROP_TEMP_RISE     "Temp_Rise"  /* 10s temp rise, decimal */

#define HUAWEI_PROP_FUSE_LEVEL    "Fuse_Level"  /*Fuse_Level  int    综合风险等级 L1~L4 */
#define HUAWEI_PROP_FUSE_SCORE    "Fuse_Score"  /*Fuse_Score  decimal 综合风险分数 0~100*/
#define HUAWEI_PROP_DOMINANT      "Dominant"  /*Dominant    string 主要风险来源 */

#define HUAWEI_DOM_STR_TEMP       "Temp/Humi"
#define HUAWEI_DOM_STR_GAS        "Gas"
#define HUAWEI_DOM_STR_BODY       "Body IR"
#define HUAWEI_DOM_STR_LIGHT      "Light"

#define HUAWEI_GAS_STR_SAFE         "Safe"      /* 未检测到烟雾/可燃气体                */
#define HUAWEI_GAS_STR_LEAK         "Leak"      /* 检测到烟雾/可燃气体                  */


#define ENABLE_HUAWEI_MODEL         1


#define UART_SEND_MAX_LEN           400
#define MQ_MAX_NUM           		4

#define True        			   	1
#define False						0


/* Wifi模块状态 */
typedef enum 
{
     ESP8285_Starting=0,
     ESP8285_Setting,   
     ESP8285_Setted_over,
     ESP8285_Connected,           
     ESP8285_Drop                 
}ESP8285_State_Value;


/*WIFI串口接收状态*/
#define WIFIUART_REC_IDLE           0
#define WIFIUART_REC_OVER           0x01U

#define  WIFI_LED_ON       
#define  WIFI_LED_OFF      
#define  ESP8285UartSend(data,len)   uart_write(uart1,len,(unsigned char *)data)




    
/* 对外提供外部变量/结构体 */
extern char qrcodeflag;
/* 对外提供外部调用函数 */
extern void ESP8285_Init(void);
extern void ESP8285_Handle(void);
extern void ESP8285_Time_Deal(void);
extern void ESP8285_Write_Data(char* data,int len);
extern void ESP8285_Read_Data(char* data,int len);
extern ESP8285_State_Value Get_ESP8285_State(void);
extern int Is_ESP8285_Connected(void);
extern void Finger_Get_Time(void);
extern void ESP8285_Finger_Get_Time(void);
#endif


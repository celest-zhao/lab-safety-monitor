/*******************************************************************************
 OC 通讯协议相关 API 函数
*******************************************************************************/

#include "usart_api.h"
#include "net.h"
#include "ocprotocol.h"
#include "stm32l4xx_hal.h"
#include "string.h"
#include "stdio.h"
#include "log.h"
#include "task_inter.h"
#include "esp8285.h"
#include "sensor_task.h"
#include "handle_config.h"

#if NET_TYPE == NET_TYPE_WIFI
#include "cJSON.h"
#include "los_memory.h"
#include "offline_cache.h"   /* 离线缓存：OFFLINE_RECORD（补传用） */
#endif


#define PTL_SEND_DATA(data,len)  Net_Module_Write_Data((char *)data,len);

/* 定义全局变量 */
PTL_SendTack_PACKAGE g_ptl_SendTack;
PTL_DATA_STRUCT g_pltSendData;
StatusStruct Scene = {1, 0};

unsigned char g_ptlSendBuf[50] = {0};
unsigned char g_ptlSendLen = 0;

unsigned char g_ptlRecBuf[100] = {0};
unsigned char g_ptlRecdLen = 0;

/* 以下为内部使用函数 */
static void ptl_Send_TimerOutDeal(PTL_SendTack_PACKAGE* sendtask);

static void ptl_TaskSendDeal(PTL_SendTack_PACKAGE* sendtask);

/*******************************************************************************
* 名    称：ptl_Send_TimerOutDeal
* 功    能：通讯发送超时检测处理
* 参数说明：sendtask —— 协议发送结构体
* 返 回 值：无
* 说    明：50ms 调用一次
* 调用方法：内部使用
*******************************************************************************/
void ptl_Send_TimerOutDeal(PTL_SendTack_PACKAGE* sendtask)
{
    if (sendtask->TackState == SendTack_Doing)
    {
        //if (++sendtask->TackTime >= 6) // 定时判断时间，60*50 = 3.0s
        if (HAL_GetTick() - sendtask->TackTime >= 1500) // 定时判断时间 1.5s
        {
            //sendtask->TackTime = 0;
            sendtask->TackTime = HAL_GetTick();
            sendtask->TackState = SendTack_Wait;//

            if(++sendtask->TimeOutCnt >= 3)// 重发 3 次后判断发送失败，暂停本任务发送
            {
                sendtask->TimeOutCnt = 0;
                sendtask->TackState = SendTack_Error;// 状态置为发送异常状态
                DEBUG_PRINT(MODULE_OC, "%s", "\r\nSend timeout 3 times,Stop!");
            }
            else
            {
                DEBUG_PRINT(MODULE_OC, "\r\nSend timeout ,%d ReSend\r\n", sendtask->TimeOutCnt);
            }
        }
    }
}
/*******************************************************************************
* 名    称：ptl_SendSuccess_Deal
* 功    能：通讯发送成功处理
* 参数说明：sendtask —— 协议发送结构体
* 返 回 值：无
* 说    明：无
* 调用方法：外部调用，在 bc35g.c 数据发送成功后调用
*******************************************************************************/
void ptl_SendSuccess_Deal(PTL_SendTack_PACKAGE* sendtask)
{
    if(sendtask->TackState == SendTack_Doing)
    {
        if(sendtask->TackNo-- <= 1)// 发送任务数 -1，如果只有一个发送任务，本任务完成后就没有任务了
        {
            sendtask->TackNo = 0;
            memset((unsigned char *)&sendtask->TackName[0], 0, 5); // 清空任务表
        }
        else
        {
            // 任务发送完成，当前任务出队，后续任务前移
            memcpy((unsigned char *)&sendtask->TackName[0], (unsigned char *)&sendtask->TackName[1],
                   sendtask->TackNo);// 任务列表前移一位
        }

        sendtask->TackTime = 0;              // 超时计时清零
        sendtask->TimeOutCnt = 0;            // 超时次数清零
        sendtask->TackState = SendTack_Wait; // 发送状态置为空闲
    }
    if (sendtask->TackState == SendTack_Error)
    {
        sendtask->TackState = SendTack_Wait;  // 发送状态置为空闲
        DEBUG_PRINT(MODULE_OC, "%s", "\r\nptl Resumes OK");
    }
}
#if NET_TYPE == NET_TYPE_NB_IOT
/*******************************************************************************
* 名    称：Hex_to_Dec
* 功    能：将一个 16 进制数据拆成指定长度的 ASCII 字符串，如单字节数据 0x36 拆为 2 个 0x33,0x36
* 参数说明：datain —— 待转换的 16 进制数据指针
           dataout —— 转换后的 ASCII 字符串指针
* 返 回 值：无
* 说    明：无
* 调用方法：内部使用
*******************************************************************************/
void Hex_to_Dec(char *datain, unsigned char Len, char* dataout)
{
    unsigned char data_buf[2];
    unsigned char Hex_Len, CurNo = 0;
    Hex_Len = Len;
    for(CurNo = 0; CurNo < Hex_Len; CurNo++) // 逐个处理收到的字符数据
    {
        data_buf[0] = *(datain + CurNo);
        data_buf[1] = *(datain + CurNo) << 4;
        if(data_buf[0] < 0xa0)
        {
            dataout[2 * CurNo] = (data_buf[0] >> 4) + 0x30;    // 数字字符编码
        }
        else
        {
            dataout[2 * CurNo] = (data_buf[0] >> 4) + 0x37;    // 大写字母字符编码
        }

        if(data_buf[1] < 0xa0)
        {
            dataout[2 * CurNo + 1] = (data_buf[1] >> 4) + 0x30;
        }
        else
        {
            dataout[2 * CurNo + 1] = (data_buf[1] >> 4) + 0x37;
        }
    }
}

/*******************************************************************************
* 名    称：ptl_send_data
* 功    能：通过网络模块发送通讯协议数据
* 参数说明：data —— 待发送数据的字符串指针
             Len  —— 待发送的字符串长度
* 返 回 值：无
* 说    明：无
* 调用方法：内部使用
*******************************************************************************/
void ptl_send_data_nb_iot( char *data, unsigned char Len)
{
    char net_data[200] = {0};
    char bc35data[250] = {0};
    unsigned char bc35Len = 13;
    
    Hex_to_Dec(data, Len, net_data);
    sprintf(bc35data, "AT+NMGS=%d,%s\r\n", Len, net_data); // 把待发送的数据拼到 AT 命令的%s 位置
    bc35Len += Len * 2;
    DEBUG_PRINT(MODULE_OC, "%s\r\n", bc35data);
    
    PTL_SEND_DATA(bc35data, bc35Len); 

}
#endif

#if NET_TYPE == NET_TYPE_WIFI

#if ENABLE_HUAWEI_MODEL
/*******************************************************************************
* 名    称：Num1_To_Str
* 功    能：浮点 -> "25.0"，保留 1 位小数的字符串（不动用 printf 的 %f）
* 参数说明：dst —— 输出缓冲（长度 >= 8 字节）；v —— 浮点值
*******************************************************************************/
static void Num1_To_Str(char *dst, float v)
{
    int neg;
    int ip, dp;

    if (dst == NULL)
    {
        return;
    }

    neg = (v < 0.0f);
    if (neg) v = -v;

    ip = (int)v;                                   /* 取整数部分          */
    dp = (int)((v - (float)ip) * 10.0f + 0.5f);     /* 十分位四舍五入      */
    if (dp >= 10) { dp -= 10; ip += 1; }            /* 9.96 -> 10.0 进位   */

    if (neg) sprintf(dst, "-%d.%d", ip, dp);
    else     sprintf(dst, "%d.%d",  ip, dp);
}

/*******************************************************************************
* 名    称：ptl_send_data_huawei
* 功    能：[改为] 按物模型拼好属性报文，通过 ESP8285 上报
* 参数说明：data —— 指向 g_ptlSendBuf（PTL_DATA_STRUCT），本函数已不再直接使用；
                    温湿度改从 sensor_task 的影子里取，其余值取自测定好的变量
            Len  —— 结构体长度，同样不再使用，因为报文长度由 JSON 自己决定
*******************************************************************************/
/* 补传节流基准：最近一次任意属性报文发出的时刻（offline_cache 读取） */
static uint32_t s_last_pub_tick = 0;
uint32_t ptl_Wifi_Last_Pub_Tick(void) { return s_last_pub_tick; }

static void ptl_send_data_huawei(char *data, unsigned char Len)
{
    char net_data[420] = {0};       /* 13 属性约 230 字节，留足余量防截断  */
    char t_str[10] = {0};
    char h_str[10] = {0};
    char l_str[8] = {0};            /* Light：光照 lx 的文本           */
    char g_str[8] = {0};            /* Gas：string 类型的文本("Safe"/"Leak") */
    char ai_str[4] = {0};          /* AI similarity 0~100 */
    char tr_str[10] = {0};          /* Temp_Rise 1 decimal */
    char fl_str[4] = {0};          /* [新增] Fuse_Level 1~4 */
    char fs_str[10] = {0};         /* [新增] Fuse_Score 1 decimal */
    char dm_str[16] = {0};         /* [新增] Dominant string */
    float t, h;
    uint16_t lux;
    uint8_t  gas, body, fill, fan;
    uint8_t  ai;
    float tr;
    uint8_t  flev;                 /* [新增] 融合等级 0~3 */
    float    fsco;                 /* [新增] 融合分数 0~100 */
    uint8_t  dom;                  /* [新增] 主要风险源 MODULE_* */

    (void)data;                     /* 未使用，保留形参是为了和旧函数同签名 */
    (void)Len;

    /* ---- 取 1s 采样任务写进影子区的各传感器值，与 LCD 上显示的是同一份数据 ---- */
    t    = Plat_GetTemp();
    h    = Plat_GetHumi();
    gas  = Plat_GetGas();           /* 1=检测到可燃/有毒气体               */
    body = Plat_GetBody();          /* 1=有人                              */
    lux  = Plat_GetLight();         /* 光照 lx                             */
    fill = Plat_GetLamp();          /* 照明执行器实际状态                  */
    fan  = Plat_GetFan();           /* 风扇执行器实际状态                  */
    ai   = Plat_GetAiSim();          /* 0~100，255=AI 未就绪 */
    tr   = Plat_GetTempRise();

    /* [新增] 融合研判结论：等级/分数/主要风险源 */
    flev = Plat_GetFuseLevel();     /* 0~3 */
    fsco = Plat_GetFuseScore();     /* 0~100 */
    dom  = Plat_GetDominant();      /* MODULE_* 0~3 */

    /* 传感器做极限保护：SHT30 读取失败会返回极端值，若不加限制，模型无法接受该区间，
       会在平台上呈现 9999 之类的脏数据并拉低历史曲线 */
    if (t < -40.0f) t = -40.0f;
    if (t >  125.0f) t =  125.0f;
    if (h < 0.0f)   h = 0.0f;
    if (h > 100.0f) h = 100.0f;
    if (lux > 54612U) lux = 0U;     /* BH1750 越界原始值（无效），按 0 处理，避免曲线掉一档 */

    Num1_To_Str(t_str, t);
    Num1_To_Str(h_str, h);
    sprintf(l_str, "%d", (int)lux);                 /* Light 是 decimal，整数文本即可 */
    sprintf(ai_str, "%d", (int)ai);
    Num1_To_Str(tr_str, tr);
    if (gas) strcpy(g_str, HUAWEI_GAS_STR_LEAK);    /* Gas 是 string，带引号上报 */
    else     strcpy(g_str, HUAWEI_GAS_STR_SAFE);

    /* [新增] 融合等级：0~3 -> L1~L4（int 属性，不带引号） */
    sprintf(fl_str, "%d", (int)flev + 1);
    /* [新增] 融合分数：0~100，1 位小数 */
    Num1_To_Str(fs_str, fsco);
    /* [新增] 主要风险源：MODULE_* -> 字符串 */
    switch (dom)
    {
        case MODULE_GAS:   strcpy(dm_str, HUAWEI_DOM_STR_GAS);   break;
        case MODULE_BODY:  strcpy(dm_str, HUAWEI_DOM_STR_BODY);  break;
        case MODULE_LIGHT: strcpy(dm_str, HUAWEI_DOM_STR_LIGHT); break;
        default:           strcpy(dm_str, HUAWEI_DOM_STR_TEMP);  break;
    }
    snprintf(net_data, sizeof(net_data),
             "{\"services\":[{\"service_id\":\"%s\",\"properties\":"
             "{\"%s\":%s,\"%s\":%s,\"%s\":\"%s\",\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":\"%s\"}}]}",
             HUAWEI_SERVICE_ID,
             HUAWEI_PROP_TEMP,    t_str,               /* decimal */
             HUAWEI_PROP_HUMI,    h_str,               /* decimal */
             HUAWEI_PROP_GAS,     g_str,               /* string，g_str 前已带好引号拼装格式 */
             HUAWEI_PROP_BODY_IR, body ? "true" : "false",
             HUAWEI_PROP_LIGHT,   l_str,               /* decimal */
             HUAWEI_PROP_SOUND,   Plat_GetSoundOn() ? "true" : "false",  /* boolean：报警声音开=true，静音关=false */
             HUAWEI_PROP_FILL,    fill ? "true" : "false",
             HUAWEI_PROP_FAN,     fan  ? "true" : "false",
             HUAWEI_PROP_AI_SCORE, ai_str,
             HUAWEI_PROP_TEMP_RISE, tr_str,
             HUAWEI_PROP_FUSE_LEVEL, fl_str,           /* [新增] int 1~4 */
             HUAWEI_PROP_FUSE_SCORE, fs_str,           /* [新增] decimal */
             HUAWEI_PROP_DOMINANT, dm_str);            /* [新增] string */

    /* 交给 net 层：串口 -> ESP8285 -> AT+MQTTPUBRAW 发布到 MQTT_PUB_TOPIC */
    s_last_pub_tick = HAL_GetTick();   /* 补传防撞车计时起点 */
    PTL_SEND_DATA(net_data, strlen(net_data));
    DEBUG_PRINT(MODULE_WIFI, "huawei up[%d]:%s\r\n", (int)strlen(net_data), net_data);
}
/*******************************************************************************
* 名    称：ptl_send_huawei_record
* 功    能：把一条离线记录打成与实时上报同格式的华为云属性报文并发布
* 说明    ：离线缓存补传专用（offline_cache.c 补传引擎逐条调用）。
*           报文格式与 ptl_send_data_huawei 完全一致 → 平台不用加任何物模型，
*           补传数据直接进「物模型数据-历史」曲线，按收到时间入库。
*           AI_Score / Temp_Rise 已随记录落盘（v4），补传时直接回放，
*           历史曲线不再出现 AI=0 的断档；AI=255 表示当时未就绪。
*******************************************************************************/
void ptl_send_huawei_record(const OFFLINE_RECORD *rec)
{
    char net_data[420] = {0};
    char t_str[10] = {0};
    char h_str[10] = {0};
    char l_str[8]  = {0};
    char g_str[8]  = {0};
    char ai_str[4] = {0};
    char tr_str[10]= {0};
    char fl_str[4] = {0};
    char fs_str[10]= {0};
    char dm_str[16]= {0};
    float t, h;

    if (rec == NULL)
    {
        return;
    }

    t = (float)rec->temp / 10.0f;      /* 记录里是 ×10 定点数 */
    h = (float)rec->humi / 10.0f;

    Num1_To_Str(t_str, t);
    Num1_To_Str(h_str, h);
    sprintf(l_str, "%d", (int)rec->light);
    sprintf(ai_str, "%d", (int)rec->ai);       /* AI_Score：0~100，255=未就绪 */
    Num1_To_Str(tr_str, (float)rec->temp_rise_x10 / 10.0f); /* Temp_Rise：x10 定点还原 */
    if (rec->gas) strcpy(g_str, HUAWEI_GAS_STR_LEAK);
    else          strcpy(g_str, HUAWEI_GAS_STR_SAFE);

    sprintf(fl_str, "%d", (int)rec->level + 1);  /* 融合等级 0~3 -> L1~L4 */
    sprintf(fs_str, "%d.0", (int)rec->score);    /* 融合分数（记录里是整数部分） */
    switch (rec->dom)                            /* 主要风险源 */
    {
        case MODULE_GAS:   strcpy(dm_str, HUAWEI_DOM_STR_GAS);   break;
        case MODULE_BODY:  strcpy(dm_str, HUAWEI_DOM_STR_BODY);  break;
        case MODULE_LIGHT: strcpy(dm_str, HUAWEI_DOM_STR_LIGHT); break;
        default:           strcpy(dm_str, HUAWEI_DOM_STR_TEMP);  break;
    }

    snprintf(net_data, sizeof(net_data),
             "{\"services\":[{\"service_id\":\"%s\",\"properties\":"
             "{\"%s\":%s,\"%s\":%s,\"%s\":\"%s\",\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":%s,\"%s\":\"%s\"}}]}",
             HUAWEI_SERVICE_ID,
             HUAWEI_PROP_TEMP,    t_str,
             HUAWEI_PROP_HUMI,    h_str,
             HUAWEI_PROP_GAS,     g_str,
             HUAWEI_PROP_BODY_IR, rec->body ? "true" : "false",
             HUAWEI_PROP_LIGHT,   l_str,
             HUAWEI_PROP_SOUND,   rec->sound ? "true" : "false",
             HUAWEI_PROP_FILL,    rec->lamp ? "true" : "false",
             HUAWEI_PROP_FAN,     rec->fan  ? "true" : "false",
             HUAWEI_PROP_AI_SCORE, ai_str,
             HUAWEI_PROP_TEMP_RISE, tr_str,
             HUAWEI_PROP_FUSE_LEVEL, fl_str,
             HUAWEI_PROP_FUSE_SCORE, fs_str,
             HUAWEI_PROP_DOMINANT, dm_str);

    s_last_pub_tick = HAL_GetTick();
    PTL_SEND_DATA(net_data, strlen(net_data));
    DEBUG_PRINT(MODULE_WIFI, "huawei replay[%d]:seq=%d\r\n", (int)strlen(net_data), (int)rec->seq);
}

#endif /* ENABLE_HUAWEI_MODEL */

/*******************************************************************************
* 名    称：ptl_send_data_wifi
* 功    能：通过网络模块发送通讯协议数据
* 参数说明：data —— 待发送数据的字符串指针
             Len  —— 待发送的字符串长度
* 返 回 值：无
* 说    明：无
* 调用方法：内部使用
*******************************************************************************/
void ptl_send_data_wifi( char *data, unsigned char Len)
{
#if ENABLE_HUAWEI_MODEL
    ptl_send_data_huawei(data, Len);

#else   /* ================= 以下为原流程，老平台上报代码，保持原样 ================= */

    char net_data[400] = {0};
    unsigned short net_len = 0;

    PTL_DATA_STRUCT* p = (PTL_DATA_STRUCT*)data;
	char temp_str[30] = {0};

    cJSON* root =  cJSON_CreateObject();
    cJSON* jservices = cJSON_CreateArray();
    cJSON* item =  cJSON_CreateObject();
    cJSON* jdata =  cJSON_CreateObject();

    cJSON_AddItemToObject(root,"services",jservices);

	#if ENABLE_HOME
	//居家参数
	
	cJSON_AddNumberToObject(jdata,"Header",p->Type);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Ver,sizeof(p->Ver));
	cJSON_AddStringToObject(jdata,"Version",temp_str);
	cJSON_AddNumberToObject(jdata,"H_Curtains_Motor_switch",p->dOutput_Curtains);
	cJSON_AddNumberToObject(jdata,"H_Gas_Bool_Input",p->dInput_Gas);
	cJSON_AddNumberToObject(jdata,"H_Body_Bool_Input",p->dInput_Body);
	cJSON_AddNumberToObject(jdata,"H_EV_Bool_OutPut_switch",p->dOutput_EV_State);
	cJSON_AddNumberToObject(jdata,"H_Bool_OutPut_switch",p->dOutput_LED_State);

	cJSON_AddNumberToObject(jdata,"H_Fan_OutPut_switch",p->dOutput_Fan_State);
	cJSON_AddNumberToObject(jdata,"H_Fan_speed_level",p->dOutput_Fan_Level);
	cJSON_AddNumberToObject(jdata,"H_Door_Servo_Output_switch",p->dOutput_Servo_State);
	cJSON_AddNumberToObject(jdata,"H_Door_Servo_Bool_Input",p->dInput_Servo_Red);
	
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Light,sizeof(p->Light));
	cJSON_AddStringToObject(jdata,"H_Light",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Temperature,sizeof(p->Temperature));
	cJSON_AddStringToObject(jdata,"H_Temperature",temp_str);
	
	cJSON_AddNumberToObject(jdata,"Error_msg",p->err_code);
	//结束
	
	cJSON_AddStringToObject(item,"service_id","Home");
	cJSON_AddItemToObject(item,"properties",jdata);
    cJSON_AddItemToArray(jservices,item);
    
	#endif
	#if ENABLE_TRAN
	
	//交通参数
	
	cJSON_AddNumberToObject(jdata,"Header",p->Type);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Ver,sizeof(p->Ver));
	cJSON_AddStringToObject(jdata,"Version",temp_str);
	cJSON_AddNumberToObject(jdata,"T_Light_Bool_Count",p->dInput_Inf);
	cJSON_AddNumberToObject(jdata,"T_Car_Bool_Input",p->dInput_Car);
	cJSON_AddNumberToObject(jdata,"T_Bool_OutPut_switch",p->dOutput_SLight_State);
	cJSON_AddNumberToObject(jdata,"T_Road_gate_Bool_switch",p->dOutput_Gate);
	cJSON_AddNumberToObject(jdata,"T_run_red_light_Bool_Input",p->dInput_Red_Light);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Light,sizeof(p->Light));
	cJSON_AddStringToObject(jdata,"T_Light",temp_str);
	cJSON_AddNumberToObject(jdata,"Error_msg",p->err_code);
	//结束
	
	cJSON_AddStringToObject(item,"service_id","Tran");
	cJSON_AddItemToObject(item,"properties",jdata);
    cJSON_AddItemToArray(jservices,item);
	
	#endif
	#if ENABLE_INDU
	//工业参数

	cJSON_AddNumberToObject(jdata,"Header",p->Type);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Ver,sizeof(p->Ver));
	cJSON_AddStringToObject(jdata,"Version",temp_str);
	cJSON_AddNumberToObject(jdata,"I_Bool_Input",p->I_Bool_Input);
	cJSON_AddNumberToObject(jdata,"I_Bool_OutPut_switch",p->I_Bool_OutPut_switch);
	cJSON_AddNumberToObject(jdata,"I_Motor_slide",p->I_Motor_slide);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->I_Temperature,sizeof(p->I_Temperature));
	cJSON_AddStringToObject(jdata,"I_Temperature",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->I_RFID_ID,sizeof(p->I_RFID_ID));
	cJSON_AddStringToObject(jdata,"I_RFID_ID",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->I_punch_the_clock_time,sizeof(p->I_punch_the_clock_time));
	cJSON_AddStringToObject(jdata,"I_punch_the_clock_time",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->I_punch_the_clock_name,sizeof(p->I_punch_the_clock_name));
	cJSON_AddStringToObject(jdata,"I_punch_the_clock_name",temp_str);
	cJSON_AddNumberToObject(jdata,"I_warehouse_gate_OutPut_switch",p->I_warehouse_gate_OutPut_switch);
    cJSON_AddNumberToObject(jdata,"I_Air_switch",p->I_Air_switch);
	cJSON_AddNumberToObject(jdata,"Error_msg",p->err_code);
	//结束
	
	cJSON_AddStringToObject(item,"service_id","Indu");
	cJSON_AddItemToObject(item,"properties",jdata);
    cJSON_AddItemToArray(jservices,item);
	#endif

	#if ENABLE_FARM
	//农业参数

	cJSON_AddNumberToObject(jdata,"Header",p->Type);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Ver,sizeof(p->Ver));
	cJSON_AddStringToObject(jdata,"Version",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Temperature,sizeof(p->Temperature));
	cJSON_AddStringToObject(jdata,"F_Temperature_Input",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Humidity,sizeof(p->Humidity));
	cJSON_AddStringToObject(jdata,"F_humidity_Input",temp_str);
	cJSON_AddNumberToObject(jdata,"F_irrigation_switch",p->dOutput_Irrigation);
	cJSON_AddNumberToObject(jdata,"F_fill_light_switch",p->dOutput_Light_Switch);
	cJSON_AddNumberToObject(jdata,"F_illumination_adjust_Input",p->dOutput_Light_Level);
	cJSON_AddNumberToObject(jdata,"F_enclosure_Output",p->dInput_Enclosure);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Carbon_Dioxide,sizeof(p->Carbon_Dioxide));
	cJSON_AddStringToObject(jdata,"F_Carbon_dioxide_Input",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Pressure,sizeof(p->Pressure));
	cJSON_AddStringToObject(jdata,"F_pressure_Input",temp_str);
	memset(temp_str,0,sizeof(temp_str));
	strncpy(temp_str,p->Light,sizeof(p->Light));
	cJSON_AddStringToObject(jdata,"F_light",temp_str);
	cJSON_AddNumberToObject(jdata,"Error_msg",p->err_code);
	//结束
	
	cJSON_AddStringToObject(item,"service_id","Farm");
	cJSON_AddItemToObject(item,"properties",jdata);
    cJSON_AddItemToArray(jservices,item);
	#endif


	char* str = cJSON_PrintUnformatted(root);
	snprintf(net_data,sizeof(net_data),"%s",str);

	if(str)
	{
		my_free(str);
	}
	if(root)
	{
		cJSON_Delete(root);
	}

    net_len = strlen(net_data);
    
    PTL_SEND_DATA(net_data, net_len); 
    DEBUG_PRINT(MODULE_WIFI,"ptl_send_data_wifi[%d]:%s\r\n",net_len,net_data);

#endif  /* ENABLE_HUAWEI_MODEL 结束 —— 老平台上报代码在这里收尾 */
}
#endif

/*******************************************************************************
* 名    称：Task0_Deal
* 功    能：发送任务 0 处理函数
* 参数说明：无
* 返 回 值：无
* 说    明：本函数为弱函数，使用时需要在 user.c 中重新定义
* 调用方法：外部使用
*******************************************************************************/
__weak void Task0_Deal(void)
{

}

/*******************************************************************************
* 名    称： Task1_Deal(void)
* 功    能：发送任务 1 处理函数
* 参数说明：无
* 返 回 值：无
* 说    明：本函数在 ocprotocol.c 中仅有弱函数定义，此处为弱定义
* 调用方法：外部调用
*******************************************************************************/
__weak void Task1_Deal(void)
{

}
/*******************************************************************************
* 名    称：Task2_Deal
* 功    能：发送任务 2 处理函数
* 参数说明：无
* 返 回 值：无
* 说    明：本函数为弱函数，使用时需要在 user.c 中重新定义
* 调用方法：外部使用
*******************************************************************************/
__weak void Task2_Deal(void)
{

}

/*******************************************************************************
* 名    称：Task3_Deal
* 功    能：发送任务 3 处理函数
* 参数说明：无
* 返 回 值：无
* 说    明：本函数为弱函数，使用时需要在 user.c 中重新定义
* 调用方法：外部使用
*******************************************************************************/
__weak void Task3_Deal(void)
{

}

/*******************************************************************************
* 名    称：ptl_Receive_OCData_Handle(char* data)
* 功    能：接收由 OC 平台下发命令的处理函数
* 参数说明：data —— 接收到的 OC 下发命令数据
* 返 回 值：无
* 说    明：具体命令的解析处理需要遵循 OC 平台与终端使用同一协议
* 调用方法：外部调用
*******************************************************************************/
__weak void ptl_Receive_OCData_Handle(char* data)
{

}

/*******************************************************************************
* 名    称：ptl_TaskSendDeal
* 功    能：通讯任务发送处理
* 参数说明：sendtask —— 协议发送结构体
* 返 回 值：无
* 说    明：无
* 调用方法：内部使用
*******************************************************************************/
void ptl_TaskSendDeal(PTL_SendTack_PACKAGE* sendtask)
{
    if(!Is_Net_Module_Connected()) // 网络模块连接状态，可由该函数判断
    {
        return;
    }
    if(sendtask->TackNo <= 0)// 发送任务数为 0，没有待执行的任务
    {
        return;
    }
    if(sendtask->TackState == SendTack_Doing)
    {
        return;
    }

    if( sendtask->TackState == SendTack_Error) // 处于异常状态，则返回
    {
        return;
    }
    else
    {
        sendtask->CurTack = sendtask->TackName[0]; // 取出待发送任务号
        sendtask->TackState = SendTack_Doing; // 状态置为发送执行状态
        sendtask->TackTime = HAL_GetTick();
    }
    /* 按不同任务进行发送处理 */
    switch(sendtask->CurTack)
    {
    case SendTask0 :
        Task0_Deal();
        break;
    case SendTask1 :
        Task1_Deal();
        break;
    case SendTask2 :
        Task2_Deal();
        break;
    case SendTask3 :
        Task3_Deal();
        break;

    default        :
        DEBUG_PRINT(MODULE_OC, "%s", "\r\nSend task error ");
        return;// 直接退出函数
    }
    #if NET_TYPE == NET_TYPE_NB_IOT
    ptl_send_data_nb_iot((char *)g_ptlSendBuf, g_ptlSendLen); // 发送数据
    #else
    ptl_send_data_wifi((char *)g_ptlSendBuf, g_ptlSendLen); // 发送数据
    #endif
    Scene.event_sendflag=0;
    g_pltSendData.err_code = 0;
}

/*******************************************************************************
* 名    称：ptl_SendTack_Init
* 功    能：通讯任务发送初始化函数
* 参数说明：sendtask —— 协议发送结构体
* 返 回 值：无
* 说    明：无
* 调用方法：内部使用
*******************************************************************************/
void ptl_SendTack_Init(PTL_SendTack_PACKAGE* sendtask)
{
    sendtask->TackNo = 1;
    sendtask->TackState = SendTack_Wait;
    sendtask->TackName[0] = SendTask1;
}

/*******************************************************************************
* 名    称：ptl_Heartbeat_Handle
* 功    能：BC35G 数据上报调用频率
* 参数说明：HeartbeatTime —— 数据上报频率，不小于 5S
* 返 回 值：无
* 说    明：1s 调用一次
           若定时器已设定时间到，开始发送数据；
           在发送数据后的第 2s 开始加载信号值
* 调用方法：外部使用
*******************************************************************************/
void ptl_Heartbeat_Handle(void)
{
    static uint32_t time = 0;
   
    if(!Is_Net_Module_Connected()) // 网络模块连接状态，可由该函数判断
    {
        return;
    }
    if((HAL_GetTick() - time >5000) || (Scene.event_sendflag == 1))
    {
          time= HAL_GetTick();
        if(Is_Net_Module_Connected())
        {
            g_ptl_SendTack.TackName[g_ptl_SendTack.TackNo] = SendTask1;
            g_ptl_SendTack.TackNo =1;       
        }
    }
}

/*******************************************************************************
* 名    称：ptl_Handle
* 功    能：通讯部分的处理函数
* 参数说明：无
* 返 回 值：无
* 说    明：500ms 调用一次
* 调用方法：外部使用
*******************************************************************************/
void ptl_Handle(void)
{
    ptl_TaskSendDeal(&g_ptl_SendTack); // 通讯协议发送处理
    ptl_Send_TimerOutDeal(&g_ptl_SendTack);
}


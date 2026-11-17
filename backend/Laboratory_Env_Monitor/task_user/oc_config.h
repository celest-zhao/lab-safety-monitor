#ifndef __oc_config_H
#define __oc_config_H
#ifdef __cplusplus
extern "C" {
#endif

/* 定义协议场景类型
 * V1.0：改用智能门禁平台（智慧家居场景）承载多传感器环境监测
 *   ENABLE_HOME = 1  家居场景 0x02 —— 平台端“基础场景 / 智能风扇 / 智能窗帘”页面
 *   ENABLE_FARM = 0  农业场景 0x06 —— V0.x 环境监测实验的旧场景，已停用
 * 注意：四个场景宏同一时刻只能开启一个，否则 PTL_DATA_STRUCT 内会重复定义
 *       Type/Ver/err_code 等字段，编译直接报错。
 */
#define ENABLE_HOME           1
#define ENABLE_INDU           0
#define ENABLE_TRAN           0
#define ENABLE_FARM           0

#ifdef __cplusplus
}
#endif
#endif /*__ oc_config_H */

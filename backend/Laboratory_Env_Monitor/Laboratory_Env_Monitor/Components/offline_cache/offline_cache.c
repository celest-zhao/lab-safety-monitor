/*******************************************************************************
  离线数据持久化（W25Q128 环形日志）+ 联网补传引擎 + 网络三态
 ******************************************************************************/

#include "offline_cache.h"
#include "w25qxx.h"
#include "rtc_api.h"
#include "sensor_task.h"
#include "stm32l4xx_hal.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* 由 ocprotocol.c 提供：最近一次任意属性报文发出的时刻（补传防撞车用） */
extern uint32_t ptl_Wifi_Last_Pub_Tick(void);
/* 由 ocprotocol.c 提供：把一条离线记录打成华为云属性报文并发布 */
extern void ptl_send_huawei_record(const OFFLINE_RECORD *rec);

/* ============================== 配置 ============================== */
#define OFF_FLASH_SECTOR_BASE   3840u   /* W25Q128 共 4096 个 4KB 扇区，取最后 512KB 作日志区 */
#define OFF_FLASH_SECTOR_NUM    128u    /* 128 个扇区 */
#define OFF_SECTOR_BYTES        4096u
#define OFF_HYSTERESIS_MS       3000u   /* 网络态滞回：连续 3 秒稳定才切换（文档 D） */
#define OFF_SAVE_PERIOD_MS      5000u   /* 离线落盘周期：5 秒一条 */
#define OFF_REPLAY_GAP_MS       1000u   /* 补传节流*/
#define OFF_ACK_TIMEOUT_MS      2000u   /* +MQTTPUB:OK 兜底超时：2 秒没等到按"重发"处理 */
#define OFF_RETRY_MAX           5u      /* 单条最大重发次数，超过视为坏条跳过 */

/* [v3 新增] 记录槽位 = 32 字节（sizeof(OFFLINE_RECORD)=28，补 4 字节对齐）。
   为什么必须对齐到 32：W25Q128 页大小 256 字节，32 能整除 256，
   于是任何一条记录都不可能跨越页边界 —— 而 w25qxx.c 的
   W25qxx_WriteSector() 在"一次写得跨页"时 pBuffer 步进写错
   （应为 += PageSize-LocalOffset，实际 += PageSize），
   会把记录后半截写成 0，crc8 校验失败后补传链路直接停摆。
   对齐后绕开这个驱动缺陷，不改动被 LCD/字库共用的 w25qxx.c。 */
#define OFF_REC_SLOT            32u
#define OFF_MAGIC               0xA6u   /* 记录格式标识：老版本/残条自动判废 */
#define OFF_SKIP_MAX            8u      /* 连续坏条最多跳过的条数 */

/* ============================== 状态 ============================== */
volatile NET_STATE g_net_state = NET_OFFLINE;

static uint8_t  s_inited    = 0;      /* W25Q128 初始化成功标志（失败则整模块旁路） */
static uint8_t  s_init_tried= 0;

static uint16_t s_w_sec     = 0;      /* 写扇区（相对号 0~127）        */
static uint16_t s_w_off     = 0;      /* 写扇区内字节偏移              */
static uint16_t s_r_sec     = 0;      /* 读扇区（最旧未补传数据）      */
static uint16_t s_r_off     = 0;      /* 读偏移                        */
static uint8_t  s_r_valid   = 0;      /* 1 = 还有未补传数据            */
static uint32_t s_seq       = 0;      /* 最近写入的序号（掉电从 Flash 恢复） */
static uint32_t s_count     = 0;      /* 未补传条数                    */

static uint32_t s_stable_ms     = 0;  /* 滞回计时                      */
static uint8_t  s_conn_stable   = 0;  /* 当前稳定判定的连通性          */
static uint32_t s_last_save_ms  = 0;  /* 上次落盘时刻                  */
static uint32_t s_offline_since = 0;  /* 进入离线的时刻（安全边界预留）*/

static uint8_t  s_wait_ack      = 0;  /* 1 = 已发出一条，等 +MQTT PUB:OK */
static uint32_t s_wait_start    = 0;
static uint8_t  s_retries       = 0;  /* 当前这条已重发次数            */

static uint8_t  s_replaying     = 0;  /* 1 = 本轮补传进行中            */
static uint32_t s_replay_start  = 0;  /* 本轮补传开始时的剩余条数      */
static uint32_t s_replay_sent   = 0;  /* 本轮已成功补传条数            */

static OFFLINE_RECORD s_secbuf_cache; /* 无符号对齐顾虑：走结构体中转 */

/* ============================== 工具 ============================== */
static uint8_t rec_crc8(const OFFLINE_RECORD *r)
{
    uint8_t crc = 0x00;
    const uint8_t *p = (const uint8_t *)r;
    uint16_t i;
    uint16_t len = (uint16_t)offsetof(OFFLINE_RECORD, crc8);
    for (i = 0; i < len; i++)
    {
        uint8_t b = p[i], k;
        crc ^= b;
        for (k = 0; k < 8; k++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static uint8_t rec_valid(const OFFLINE_RECORD *r)
{
    if (r->seq == 0xFFFFFFFFu)            return 0;   /* 擦除态          */
    if (r->reserved != OFF_MAGIC)         return 0;   /* [v3] 非本版格式 */
    if (r->crc8 != rec_crc8(r))           return 0;   /* 半写/损坏       */
    if (r->type != 0x01 && r->type != 0x02) return 0;
    return 1;
}

/* RTC 字符串 "YYYY/MM/DD,HH:MM:SS" -> 自 2000-01-01 起的秒数（仅用于记录时间轴） */
static uint32_t rtc_to_secs(void)
{
    char buf[24] = {0};
    int y, mo, d, h, mi, se;
    uint32_t days, a, b;
    rtc_gettime(buf);
    if (buf[0] < '0' || buf[0] > '9') return 0;
    y  = (buf[0]-'0')*1000 + (buf[1]-'0')*100 + (buf[2]-'0')*10 + (buf[3]-'0');
    mo = (buf[5]-'0')*10 + (buf[6]-'0');
    d  = (buf[8]-'0')*10 + (buf[9]-'0');
    h  = (buf[11]-'0')*10 + (buf[12]-'0');
    mi = (buf[14]-'0')*10 + (buf[15]-'0');
    se = (buf[17]-'0')*10 + (buf[18]-'0');
    if (mo < 1) mo = 1;  if (mo > 12) mo = 12;
    if (d  < 1) d  = 1;  if (d  > 31) d  = 31;
    if (y  < 2000) y = 2000;
    /* days_from_civil 算法（y/m/d -> 距 1970-01-01 天数），再减 2000 基准 */
    a = (14u - (uint32_t)mo) / 12u;
    b = (uint32_t)y + 4800u - a;
    days = ((uint32_t)y*365u + b/4u - b/100u + b/400u
            + (153u*((uint32_t)mo + 12u*a - 3u) + 2u)/5u + (uint32_t)d - 1u)
           - 10957u;    /* 2000-01-01 距 1970-01-01 的天数 */
    return days*86400u + (uint32_t)h*3600u + (uint32_t)mi*60u + (uint32_t)se;
}

/* ============================== 读指针辅助 ============================== */
/* 从 from 扇区（相对号）开始向后找第一个含有效记录的扇区，找不到返回 0 */
static uint8_t seek_read_sector(uint16_t from, uint16_t *out_sec)
{
    uint16_t k, step;
    static uint8_t head[OFF_SECTOR_BYTES];
    for (step = 0; step < OFF_FLASH_SECTOR_NUM; step++)
    {
        k  = (uint16_t)((from + step) % OFF_FLASH_SECTOR_NUM);
        if (k == s_w_sec && s_w_off == 0) continue;      /* 写指针前空的当前扇区 */
        W25qxx_ReadSector(head, OFF_FLASH_SECTOR_BASE + k, 0, OFF_SECTOR_BYTES);
        memcpy(&s_secbuf_cache, head, sizeof(OFFLINE_RECORD));
        if (rec_valid(&s_secbuf_cache))
        {
            *out_sec = k;
            return 1;
        }
    }
    return 0;
}

static void replay_end(const char *reason)
{
    if (s_replaying)
    {
        DEBUG_PRINT(MODULE_E2PROOM, "offline cache: replay done, total=%d (%s)\r\n",
                    (int)s_replay_sent, reason ? reason : "");
    }
    s_replaying   = 0;
    s_replay_sent = 0;
}

/* ============================== 初始化 ============================== */
void OfflineCache_Init(void)
{
    uint16_t k;
    uint32_t best_seq = 0, total = 0;
    int16_t  w_found  = -1;
    uint16_t w_off_found = 0;
    static uint8_t scanbuf[OFF_SECTOR_BYTES];

    if (s_init_tried) return;
    s_init_tried = 1;

    if (!W25qxx_Init())
    {
        DEBUG_PRINT(MODULE_E2PROOM, "%s", "offline cache: W25Q init FAIL, bypass\r\n");
        return;                       /* Flash 不可用：整模块旁路，主流程不受影响 */
    }
    if (w25qxx.SectorCount < (OFF_FLASH_SECTOR_BASE + OFF_FLASH_SECTOR_NUM))
    {
        DEBUG_PRINT(MODULE_E2PROOM, "%s", "offline cache: flash too small\r\n");
        return;
    }

    /* 上电扫描（掉电恢复，文档 A-4）：逐扇区找最后一条完整记录 */
    for (k = 0; k < OFF_FLASH_SECTOR_NUM; k++)
    {
        uint32_t off = 0;
        uint32_t last_seq = 0;
        uint8_t  any = 0;
        W25qxx_ReadSector(scanbuf, OFF_FLASH_SECTOR_BASE + k, 0, OFF_SECTOR_BYTES);
        while (off + OFF_REC_SLOT <= OFF_SECTOR_BYTES)
        {
            OFFLINE_RECORD r;
            memcpy(&r, scanbuf + off, sizeof(r));
            if (!rec_valid(&r)) break;
            last_seq = r.seq;
            any = 1;
            off += OFF_REC_SLOT;
        }
        if (any)
        {
            total++;    /* 该扇区有数据（扇区数即参与总量的单位，精确条数在下面累加） */
            if ((int32_t)last_seq > (int32_t)best_seq)
            {
                best_seq    = last_seq;
                w_found     = (int16_t)k;
                w_off_found = (uint16_t)off;
            }
        }
    }
    (void)total;

    if (w_found < 0)
    {
        /* 全空：擦 0 号扇区从头开始 */
        s_w_sec = 0; s_w_off = 0; s_seq = 0;
        W25qxx_EraseSector(OFF_FLASH_SECTOR_BASE + 0);
        s_r_valid = 0; s_count = 0;
        DEBUG_PRINT(MODULE_E2PROOM, "%s", "offline cache: empty log\r\n");
    }
    else
    {
        uint16_t k2;
        s_w_sec = (uint16_t)w_found;
        s_w_off = w_off_found;
        s_seq   = best_seq;
        /* 统计未消费条数：从写扇区结尾向前逐扇区数有效记录 */
        s_count = 0;
        for (k2 = 0; k2 < OFF_FLASH_SECTOR_NUM; k2++)
        {
            uint32_t off = 0;
            W25qxx_ReadSector(scanbuf, OFF_FLASH_SECTOR_BASE + k2, 0, OFF_SECTOR_BYTES);
            while (off + OFF_REC_SLOT <= OFF_SECTOR_BYTES)
            {
                OFFLINE_RECORD r;
                memcpy(&r, scanbuf + off, sizeof(r));
                if (!rec_valid(&r)) break;
                s_count++;
                off += OFF_REC_SLOT;
            }
        }
        /* 读指针 = 最旧数据所在扇区（从写扇区下一个开始找） */
        if (seek_read_sector((uint16_t)((s_w_sec + 1) % OFF_FLASH_SECTOR_NUM), &s_r_sec))
        {
            s_r_off   = 0;
            s_r_valid = 1;
        }
        else
        {
            s_r_valid = 0;
        }
        DEBUG_PRINT(MODULE_E2PROOM, "offline cache: resume w_sec=%d w_off=%d seq=%d cnt=%d\r\n",
                    (int)s_w_sec, (int)s_w_off, (int)s_seq, (int)s_count);
    }

    s_inited = 1;
}

/* ============================== 写入 ============================== */
#define OFF_REC_PER_SECTOR  (OFF_SECTOR_BYTES / OFF_REC_SLOT)   /* 4096/32 = 128 条/扇区 */

static uint8_t cache_save(const OFFLINE_RECORD *rec)
{
    OFFLINE_RECORD r = *rec;
    static uint8_t slot[OFF_REC_SLOT];
    uint16_t wr_off;

    if (!s_inited) return 0;

    /* 扇区写满 -> 换下一个扇区（先擦再写）；若那个扇区还有未补传数据，
       说明环已绕回，按"覆盖最旧"语义把读指针跳过该扇区（数据被覆盖丢弃） */
    if (s_w_off + OFF_REC_SLOT > OFF_SECTOR_BYTES)
    {
        uint16_t nxt = (uint16_t)((s_w_sec + 1) % OFF_FLASH_SECTOR_NUM);
        if (s_r_valid && s_r_sec == nxt)
        {
            uint16_t ns;
            if (seek_read_sector((uint16_t)((nxt + 1) % OFF_FLASH_SECTOR_NUM), &ns))
            {
                s_r_sec = ns; s_r_off = 0;
                /* 被覆盖扇区里的记录条数粗略扣减，避免 Count 虚高 */
                s_count = (s_count > OFF_REC_PER_SECTOR) ? (s_count - OFF_REC_PER_SECTOR) : 0;
            }
            else
            {
                s_r_valid = 0; s_count = 0;
            }
        }
        W25qxx_EraseSector(OFF_FLASH_SECTOR_BASE + nxt);
        s_w_sec = nxt;
        s_w_off = 0;
    }

    wr_off = s_w_off;                 /* 本条即将写入的物理位置（槽位对齐） */
    r.seq      = ++s_seq;
    r.reserved = OFF_MAGIC;           /* [v3] 写入格式标识 */
    r.crc8     = rec_crc8(&r);

    memset(slot, 0, sizeof(slot));    /* 槽位尾部清零，便于肉眼 dump 辨认 */
    memcpy(slot, &r, sizeof(r));
    W25qxx_WriteSector(slot, OFF_FLASH_SECTOR_BASE + s_w_sec, wr_off, OFF_REC_SLOT);

    s_w_off += (uint16_t)OFF_REC_SLOT;
    s_count++;

    if (!s_r_valid)
    {
        s_r_sec   = s_w_sec;
        s_r_off   = wr_off;
        s_r_valid = 1;
    }
    return 1;
}

/* ============================== 读出 ============================== */
static uint8_t cache_peek(OFFLINE_RECORD *out)
{
    static uint8_t rbuf[OFF_SECTOR_BYTES];
    uint8_t skipped = 0;

    if (!s_inited || !s_r_valid) return 0;

    for (;;)
    {
        if (s_r_sec == s_w_sec && s_r_off >= s_w_off)
        {
            s_r_valid = 0;                 /* 补传完成               */
            s_count   = 0;
            replay_end("caught up");       /* [v2/v3] 打印总条数 */
            return 0;
        }
        W25qxx_ReadSector(rbuf, OFF_FLASH_SECTOR_BASE + s_r_sec, s_r_off, OFF_REC_SLOT);
        memcpy(out, rbuf, sizeof(*out));
        if (rec_valid(out))
        {
            return 1;
        }

      
        s_r_off += OFF_REC_SLOT;
        if (s_count) s_count--;
        if (++skipped >= OFF_SKIP_MAX)
        {
            s_r_valid = 0;
            s_count   = 0;
            replay_end("bad rec");
            return 0;
        }
        if (s_r_off + OFF_REC_SLOT > OFF_SECTOR_BYTES)
        {
            /* 本扇区扫完：回收并换下一个有数据的扇区 */
            uint16_t ns;
            if (s_r_sec != s_w_sec)
            {
                W25qxx_EraseSector(OFF_FLASH_SECTOR_BASE + s_r_sec);
            }
            if (seek_read_sector((uint16_t)((s_r_sec + 1) % OFF_FLASH_SECTOR_NUM), &ns))
            {
                s_r_sec = ns; s_r_off = 0;
            }
            else
            {
                s_r_valid = 0;
                s_count   = 0;
                replay_end("no more");
                return 0;
            }
        }
    }
}

static void cache_pop(void)
{
    if (!s_inited || !s_r_valid) return;
    s_r_off += (uint16_t)OFF_REC_SLOT;
    if (s_count) s_count--;
    if (s_replaying)
    {
        s_replay_sent++;
        if ((s_replay_sent % 10u) == 0u)
        {
            DEBUG_PRINT(MODULE_E2PROOM, "offline cache: replay progress %d/%d left=%d\r\n",
                        (int)s_replay_sent, (int)s_replay_start, (int)s_count);
        }
    }

    if (s_r_off + OFF_REC_SLOT > OFF_SECTOR_BYTES ||
        (s_r_sec == s_w_sec && s_r_off >= s_w_off))
    {
        /* 本扇区消费完：擦掉回收（写扇区除外，它还在追加），找下一个有数据的扇区 */
        if (s_r_sec != s_w_sec)
        {
            W25qxx_EraseSector(OFF_FLASH_SECTOR_BASE + s_r_sec);
        }
        if (seek_read_sector((uint16_t)((s_r_sec + 1) % OFF_FLASH_SECTOR_NUM), &s_r_sec))
        {
            s_r_off = 0;
            s_r_valid = 1;
        }
        else
        {
            s_r_valid = 0;
            s_count   = 0;
        }
    }
}

/* ============================== 采集一条当前快照 ============================== */
static uint8_t capture_record(OFFLINE_RECORD *r)
{
    float t  = Plat_GetTemp();
    float h  = Plat_GetHumi();
    float sc = Plat_GetFuseScore();
    uint16_t lux = Plat_GetLight();
    if (t == 0.0f && h == 0.0f)
    {
        return 0;
    }

    memset(r, 0, sizeof(*r));
    r->ts    = rtc_to_secs();
    r->level = Plat_GetFuseLevel();
    r->type  = (r->level >= 2) ? 0x02 : 0x01;   /* 告警事件级 单独标记（文档 E-2） */
    if (t < -40.0f)  t = -40.0f;
    if (t >  125.0f) t =  125.0f;
    if (h < 0.0f)    h = 0.0f;
    if (h > 100.0f)  h = 100.0f;
    if (lux > 54612u) lux = 0;
    r->temp  = (int16_t)(t * 10.0f);
    r->humi  = (int16_t)(h * 10.0f);
    r->light = lux;
    r->gas   = Plat_GetGas();
    r->body  = Plat_GetBody();
    r->fan   = Plat_GetFan();
    r->lamp  = Plat_GetLamp();
    r->dom   = Plat_GetDominant();
    r->sound = Plat_GetSoundOn();
    if (sc < 0.0f)   sc = 0.0f;
    if (sc > 100.0f) sc = 100.0f;
    r->score = (uint8_t)sc;

    /* AI 相似度与温升一并落盘，离线补传不再写 0 */
    r->ai = Plat_GetAiSim();
    {
        float tr = Plat_GetTempRise();
        if (tr < -12.7f) tr = -12.7f;
        if (tr >  12.7f) tr =  12.7f;
        r->temp_rise_x10 = (int8_t)(tr * 10.0f);
    }
    return 1;
}

/* ============================== 对外接口 ============================== */
void OfflineCache_Poll(uint8_t net_connected)
{
    uint32_t now = HAL_GetTick();

    if (!s_init_tried)
    {
        OfflineCache_Init();
    }
    if (!s_inited) return;

    /* ---- 1) 网络态滞回切换（文档 D：3 秒去抖，防闪断抖动继电器） ---- */
    if ((uint8_t)s_conn_stable != (net_connected ? 1u : 0u))
    {
        if (s_stable_ms == 0)
        {
            s_stable_ms = now;
        }
        else if (now - s_stable_ms >= OFF_HYSTERESIS_MS)
        {
            s_conn_stable = net_connected ? 1u : 0u;
            s_stable_ms   = 0;
            g_net_state   = s_conn_stable ? NET_ONLINE : NET_OFFLINE;
            if (!s_conn_stable)
            {
                s_offline_since = now;      /* 预留：离线执行器安全边界的计时起点（文档 E） */
                DEBUG_PRINT(MODULE_E2PROOM, "%s", "offline cache: net -> OFFLINE\r\n");
            }
            else
            {             
                DEBUG_PRINT(MODULE_E2PROOM, "offline cache: net -> ONLINE, pending=%d\r\n",
                            (int)s_count);               
                if (!s_r_valid && s_count > 0)
                {
                    uint16_t ns;
                    if (seek_read_sector((uint16_t)((s_w_sec + 1) % OFF_FLASH_SECTOR_NUM), &ns))
                    {
                        s_r_sec   = ns;
                        s_r_off   = 0;
                        s_r_valid = 1;
                    }
                    else if (s_w_off >= OFF_REC_SLOT)
                    {
                        s_r_sec   = s_w_sec;
                        s_r_off   = 0;
                        s_r_valid = 1;
                    }
                    else
                    {
                        s_count = 0;
                    }
                }
            }
        }
    }
    else
    {
        s_stable_ms = 0;
    }
    (void)s_offline_since;

    /* 离线：按周期落盘（文档 A：在线直发不写 Flash） ---- */
    if (g_net_state != NET_ONLINE)
    {
        if (now - s_last_save_ms >= OFF_SAVE_PERIOD_MS)
        {
            OFFLINE_RECORD r;
            s_last_save_ms = now;
            if (capture_record(&r) && cache_save(&r))
            {
                DEBUG_PRINT(MODULE_E2PROOM, "offline cache: save seq=%d cnt=%d\r\n",
                            (int)r.seq, (int)s_count);
            }
        }
        s_wait_ack = 0;                 /* 掉线期间的 ACK 状态作废，恢复后重来 */
        /* 掉线时补传中断：打印已传/剩余，恢复联网后重新计数 */
        if (s_replaying)
        {
            DEBUG_PRINT(MODULE_E2PROOM, "offline cache: replay pause sent=%d left=%d\r\n",
                        (int)s_replay_sent, (int)s_count);
            s_replaying   = 0;
            s_replay_sent = 0;
        }
        return;
    }

    /* ---- 3) 在线：补传引擎（文档 B：顺序重放 + ACK 推进 + 节流） ---- */
    if (s_wait_ack && (now - s_wait_start > OFF_ACK_TIMEOUT_MS))
    {
     
        s_wait_ack = 0;
        s_retries++;
        if (s_retries >= OFF_RETRY_MAX)
        {
            DEBUG_PRINT(MODULE_E2PROOM, "offline cache: no ack x%d, skip seq=%d\r\n",
                        (int)s_retries, (int)s_count);
            cache_pop();
            s_retries = 0;
        }
    }

   
    if (!s_wait_ack && s_r_valid &&
        (now - ptl_Wifi_Last_Pub_Tick()) >= OFF_REPLAY_GAP_MS)
    {
        OFFLINE_RECORD r;
        if (cache_peek(&r))
        {
            /* [v2 新增] 一轮补传开始时打印计划条数（只打一次） */
            if (!s_replaying)
            {
                s_replaying    = 1;
                s_replay_start = s_count;
                s_replay_sent  = 0;
                DEBUG_PRINT(MODULE_E2PROOM, "offline cache: replay start cnt=%d (1 rec/s)\r\n",
                            (int)s_replay_start);
            }
            ptl_send_huawei_record(&r);
            s_wait_start = now;
            s_wait_ack   = 1;
        }
        else
        {
            s_wait_ack = 0;
        }
    }
}

void OfflineCache_NotifyPub(uint8_t ok)
{
    if (!s_inited || !s_wait_ack) return;

    if (ok)
    {
        cache_pop();                    /* 确认成功才推进读指针（文档 B-3） */
        s_retries  = 0;
        s_wait_ack = 0;
    }
    else
    {
        s_wait_ack = 0;                 /* 失败：原地重发同一条 */
        s_retries++;
        if (s_retries >= OFF_RETRY_MAX)
        {
            DEBUG_PRINT(MODULE_E2PROOM, "offline cache: drop bad rec after %d retries\r\n",
                        (int)s_retries);
            cache_pop();
            s_retries = 0;
        }
    }
}

uint32_t OfflineCache_Count(void)   { return s_count; }
NET_STATE OfflineCache_GetNetState(void) { return g_net_state; }

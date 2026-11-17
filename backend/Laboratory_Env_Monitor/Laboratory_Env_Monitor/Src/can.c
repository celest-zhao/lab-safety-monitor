/**
  ******************************************************************************
  * File Name          : CAN.c
  * Description        : This file provides code for the configuration
  *                      of the CAN instances.
  ******************************************************************************
  ** This notice applies to any and all portions of this file
  * that are not between comment pairs USER CODE BEGIN and
  * USER CODE END. Other portions of this file, whether 
  * inserted by the user or by software development tools
  * are owned by their respective copyright owners.
  *
  * COPYRIGHT(c) 2019 STMicroelectronics
  *
  * Redistribution and use in source and binary forms, with or without modification,
  * are permitted provided that the following conditions are met:
  *   1. Redistributions of source code must retain the above copyright notice,
  *      this list of conditions and the following disclaimer.
  *   2. Redistributions in binary form must reproduce the above copyright notice,
  *      this list of conditions and the following disclaimer in the documentation
  *      and/or other materials provided with the distribution.
  *   3. Neither the name of STMicroelectronics nor the names of its contributors
  *      may be used to endorse or promote products derived from this software
  *      without specific prior written permission.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "can.h"
#include "can_api.h"
/* USER CODE BEGIN 0 */
CAN_TxHeaderTypeDef   TxHeader;
CAN_RxHeaderTypeDef   RxHeader;
uint8_t               TxData[8];
uint8_t               RxData[8];
uint32_t              TxMailbox;
/* USER CODE END 0 */
void CAN_Config(void);
static void MX_CAN1_Init(uint32_t Prescaler);
CAN_HandleTypeDef hcan1;
__weak void can_error_handle(void);
/*

时钟为80M
tq=1/(80/Prescaler)M
假设默认BS1=3tq;BS2=4tq.
tq+tBS1+tBS2=8tq

所以：Baurate=1/(8*tq)=10/Prescaler MHZ
波特率范围：125K~1M
*/
void can_open(uint32_t BaudRate)
{
    uint32_t Prescaler=0;
    
    Prescaler = 10000000/BaudRate;
    MX_CAN1_Init(Prescaler);
    CAN_Config();
}
/* CAN1 init function */
void MX_CAN1_Init(uint32_t Prescaler)
{
  hcan1.Instance = CAN1;
  hcan1.Init.Prescaler = 79;
  hcan1.Init.Mode = CAN_MODE_NORMAL;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = CAN_BS1_3TQ;
  hcan1.Init.TimeSeg2 = CAN_BS2_4TQ;
  hcan1.Init.TimeTriggeredMode = DISABLE;
  hcan1.Init.AutoBusOff = DISABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = DISABLE;
  hcan1.Init.ReceiveFifoLocked = DISABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    can_error_handle();
  }
}

void HAL_CAN_MspInit(CAN_HandleTypeDef* canHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(canHandle->Instance==CAN1)
  {
  /* USER CODE BEGIN CAN1_MspInit 0 */

  /* USER CODE END CAN1_MspInit 0 */
    /* CAN1 clock enable */
    __HAL_RCC_CAN1_CLK_ENABLE();
  
    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**CAN1 GPIO Configuration    
    PA11     ------> CAN1_RX
    PA12     ------> CAN1_TX 
    */
    GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN CAN1_MspInit 1 */
    /* CAN1 interrupt Init */
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
  /* USER CODE END CAN1_MspInit 1 */
  }
}

void HAL_CAN_MspDeInit(CAN_HandleTypeDef* canHandle)
{

  if(canHandle->Instance==CAN1)
  {
  /* USER CODE BEGIN CAN1_MspDeInit 0 */

  /* USER CODE END CAN1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_CAN1_CLK_DISABLE();
  
    /**CAN1 GPIO Configuration    
    PA11     ------> CAN1_RX
    PA12     ------> CAN1_TX 
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11|GPIO_PIN_12);

  /* USER CODE BEGIN CAN1_MspDeInit 1 */

  /* USER CODE END CAN1_MspDeInit 1 */
  } 
} 

/* USER CODE BEGIN 1 */

void CAN_Config(void)
{
  CAN_FilterTypeDef  sFilterConfig;

  /*##-2- Configure the CAN Filter ###########################################*/
  sFilterConfig.FilterBank = 0;//一共有28组筛选器，设置使用哪个筛选组
  sFilterConfig.FilterMode = CAN_FILTERMODE_IDMASK; //筛选器两个 32 位寄存器处于标识符掩码模式
  sFilterConfig.FilterScale = CAN_FILTERSCALE_32BIT;//尺度为一个32位寄存器
  sFilterConfig.FilterIdHigh = 0x0000;//设置筛选器ID的高位数值
  sFilterConfig.FilterIdLow = 0x0000;//设置筛选器ID的低位数值
  sFilterConfig.FilterMaskIdHigh = 0x0000;//设置筛选器掩码ID的高位数值 写0表示筛选位不需要匹配，全部为0则说明筛选器都可不匹配，可理解为所有消息都通过
  sFilterConfig.FilterMaskIdLow = 0x0000;//设置筛选器掩码ID的低位数值 
  sFilterConfig.FilterFIFOAssignment = CAN_RX_FIFO0;//通过此筛选器的消息将存储在对应筛选器的 FIFO-0
  sFilterConfig.FilterActivation = ENABLE;//激活筛选器
//  sFilterConfig.SlaveStartFilterBank = 14; //对于只有单个CAN的mcu，此参数无意义

  if (HAL_CAN_ConfigFilter(&hcan1, &sFilterConfig) != HAL_OK)
  {
    /* Filter configuration Error */
    can_error_handle();
  }

  /*##-3- Start the CAN peripheral ###########################################*/
  if (HAL_CAN_Start(&hcan1) != HAL_OK) // 启动CAN
  {
    /* Start Error */
    can_error_handle();
  }

  /*##-4- Activate CAN RX notification #######################################*/
  if (HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) //激活CAN接受数据通知
  {
    /* Notification Error */
    can_error_handle();
  }

  /*##-5- Configure Transmission process #####################################初始化数据帧的仲裁段和控制段*/
//  TxHeader.StdId = 0x321;//初始化发送数据的ID
//  TxHeader.ExtId = 0x01;//扩展ID
//  TxHeader.RTR = CAN_RTR_DATA;//数据帧（另外一种是遥控帧）
//  TxHeader.IDE = CAN_ID_STD;//标准11位ID 
//  TxHeader.DLC = 2;//数据长度2字节
  TxHeader.TransmitGlobalTime = DISABLE;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    if(HAL_CAN_GetRxMessage(&hcan1,CAN_RX_FIFO0,&RxHeader,RxData) != HAL_OK)
    {
        //error
    } 
}
void can_write(uint16_t length,uint8_t* ptrdata)
{
//    TxData[0] = *ptrdata;
//    TxData[1] = *(ptrdata+1);  
  TxHeader.StdId = 0x321;//初始化发送数据的ID
  TxHeader.ExtId = 0x01;//扩展ID
  TxHeader.RTR = CAN_RTR_DATA;//数据帧（另外一种是遥控帧）
  TxHeader.IDE = CAN_ID_STD;//标准11位ID 
  TxHeader.DLC = length;//数据长度2字节  
    if((HAL_CAN_AddTxMessage(&hcan1,&TxHeader,ptrdata,&TxMailbox) != HAL_OK))
    {
        //error  
    }
}

void can_read(uint16_t *length,uint8_t *pbuffer)
{
    
    if((RxHeader.StdId == 0x321)&&(RxHeader.IDE == CAN_ID_STD)&&(RxHeader.DLC == 2))
    {
        *length = RxHeader.DLC;
        for(char i=0;i<RxHeader.DLC;i++)
        *(pbuffer+i)=RxData[i];
    }

}
/* USER CODE END 1 */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    i2c.c
  * @brief   This file provides code for the configuration
  *          of the I2C instances.
  ******************************************************************************
  * @attention
  *
  * 2.3 章节新增（外部存储器）：I2C1 挂开发板 U13 = AT24C02 EEPROM。
  *   PB8 = IIC_SCL、PB9 = IIC_SDA（AF4），Fast Mode 400 kHz，
  *   板载 R33/R34 上拉；A2/A1/A0 接地 → 器件地址 0xA0（写）/0xA1（读）。
  *   本文件按 CubeMX 生成格式手写（与 usart.c 同一约定）：
  *   MX_I2C1_Init 由 main.c 无条件调用 —— cfg 开关关掉的是"用总线的模块"，
  *   总线本身属于 Core 公共外设（与关 MOD_WIFI 后 USART3 仍初始化一致），
  *   没人用时 hi2c1 与 HAL_I2C 代码会被 --gc-sections 自动回收。
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "i2c.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

I2C_HandleTypeDef hi2c1;

/* I2C1 init function */

void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  /* ★ 100 kHz 而不是 400 kHz（2026-10-29 真机决策）：
   *   本板 EEPROM 挂在 PB8/PB9，板上是否有 4.7k 外部上拉不确定；
   *   只用片内 40k 弱上拉时 400 kHz 边沿爬升不足，会出现"probe 能 ACK、
   *   连续读写却偶发 NACK/TIMEOUT"的间歇性故障。AT24Cxx 全系支持 100 kHz，
   *   存储链路不在乎这点带宽 —— 用稳定性换速度。
   *  （这一行在 USER CODE 之外：CubeMX 重新生成会把它改回 400000，需重新核对） */
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* 模拟/数字滤波器（HAL_I2CEx_ConfigAnalog/DigitalFilter）不调用：
   * 本工程的 CMSIS 器件头没有 FLTR 寄存器定义，Ex 扩展整段编译为空；
   * F4 复位默认 ANFOFF=0/DNF=0，短走线 400 kHz 实测够用，留注释为证 */
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

void HAL_I2C_MspInit(I2C_HandleTypeDef* i2cHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(i2cHandle->Instance==I2C1)
  {
  /* USER CODE BEGIN I2C1_MspInit 0 */

  /* USER CODE END I2C1_MspInit 0 */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**I2C1 GPIO Configuration
    PB8     ------> I2C1_SCL
    PB9     ------> I2C1_SDA
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* Peripheral clock enable */
    __HAL_RCC_I2C1_CLK_ENABLE();

    /* I2C1 interrupt Init —— 本工程 I2C 全部走阻塞（轮询）模式，
     * 事件/错误中断不使能：NVIC 不开就没有中断入口，两个 handler 无需进 stm32f4xx_it.c */
  /* USER CODE BEGIN I2C1_MspInit 1 */

  /* USER CODE END I2C1_MspInit 1 */
  }
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef* i2cHandle)
{

  if(i2cHandle->Instance==I2C1)
  {
  /* USER CODE BEGIN I2C1_MspDeInit 0 */

  /* USER CODE END I2C1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_I2C1_CLK_DISABLE();

    /**I2C1 GPIO Configuration
    PB8     ------> I2C1_SCL
    PB9     ------> I2C1_SDA
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_8|GPIO_PIN_9);

  /* USER CODE BEGIN I2C1_MspDeInit 1 */

  /* USER CODE END I2C1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

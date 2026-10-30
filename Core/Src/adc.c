/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    adc.c
  * @brief   ADC1 初始化（2.8 光敏传感器链新增：PA5 = ADC1_IN5）
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "adc.h"

/* USER CODE BEGIN 0 */
/* 为什么这个文件是手写而不是 CubeMX 生成：
 *   本工程的 .ioc 里没有 ADC 外设，重新生成代码不会产出本文件；
 *   但它与 usart.c / spi.c 同属"Core 公共外设"，main() 里无条件初始化，
 *   cfg 开关关掉的是"用 ADC 的模块"（per_light），没人用时 HAL 代码被 gc 回收。
 * ★ 用 CubeMX 重新生成时本文件不会被删除（不在生成清单里），但若在 .ioc
 *   里新增 ADC 配置，生成物会覆盖本文件——届时按源码的 3 个改动点（本段注释、
 *   MX_ADC1_Init 的初始化参数、CFG 行以下的中文注释）手工合并即可。 */
/* USER CODE END 0 */

ADC_HandleTypeDef hadc1;

/* ADC1 init function */
void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  * ★ ADCCLK = PCLK2(84 MHz) / 4 = 21 MHz（F407 的 ADC 最高时钟 36 MHz，21 MHz 留足余量）
  * ★ 软件触发 + 单次转换 + EOC 单次：查询式采集，不用 DMA / 不用中断
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  * ★ 采样 3 个 ADC 周期：转换时间 ≈ (3 + 12) / 21 MHz ≈ 0.7 µs，
  *   远小于光敏需要的响应速度；采样时间越长抗噪越好，但本模块是直流分压，不必等。
  */
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

void HAL_ADC_MspInit(ADC_HandleTypeDef* adcHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(adcHandle->Instance==ADC1)
  {
  /* USER CODE BEGIN ADC1_MspInit 0 */

  /* USER CODE END ADC1_MspInit 0 */
    /* ADC1 clock enable */
    __HAL_RCC_ADC1_CLK_ENABLE();

  /* USER CODE BEGIN ADC1_MspInit 1 */
  /* PA5 必须配成模拟模式：数字输入缓冲关闭，否则引脚上的数字门会给分压点
   * 带来额外负载，读数会系统性偏低。GPIO_NOPULL = 不用内部上下拉，
   * 分压由板上固定电阻 + 光敏电阻决定，内部电阻一挂就把分压比改了。 */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  /**ADC1 GPIO Configuration
  PA5     ------> ADC1_IN5
  */
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
  /* USER CODE END ADC1_MspInit 1 */
  }
}

void HAL_ADC_MspDeInit(ADC_HandleTypeDef* adcHandle)
{

  if(adcHandle->Instance==ADC1)
  {
  /* USER CODE BEGIN ADC1_MspDeInit 0 */

  /* USER CODE END ADC1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_ADC1_CLK_DISABLE();

    /**ADC1 GPIO Configuration
    PA5     ------> ADC1_IN5
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_5);

  /* USER CODE BEGIN ADC1_MspDeInit 1 */

  /* USER CODE END ADC1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

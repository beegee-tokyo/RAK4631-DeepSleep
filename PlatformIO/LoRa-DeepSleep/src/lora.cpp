/**
 * @file lora.cpp
 * @author Bernd Giesecke (bernd.giesecke@rakwireless.com)
 * @brief LoRa init, send and receive functions
 * @version 0.1
 * @date 2021-01-03
 * 
 * @copyright Copyright (c) 2021
 * 
 */

#include "main.h"

#define TX_ONLY

// LoRa transmission settings
#define RF_FREQUENCY 923300000	// Hz
#define TX_OUTPUT_POWER 22		// dBm
#define LORA_BANDWIDTH 0		// 0: 125 kHz, 1: 250 kHz, 2: 500 kHz, 3: Reserved
#define LORA_SPREADING_FACTOR 7 // SF7..SF12
#define LORA_CODINGRATE 1		// 1: 4/5, 2: 4/6, 3: 4/7, 4: 4/8
#define LORA_PREAMBLE_LENGTH 8	// Same for Tx and Rx
#define LORA_SYMBOL_TIMEOUT 0	// Symbols
#define LORA_FIX_LENGTH_PAYLOAD_ON false
#define LORA_IQ_INVERSION_ON false
#define TX_TIMEOUT_VALUE 5000

// DIO1 pin on RAK4631
#define PIN_LORA_DIO_1 47

// LoRa callbacks
static RadioEvents_t RadioEvents;
void OnTxDone(void);
void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr);
void OnTxTimeout(void);
void OnRxTimeout(void);
void OnRxError(void);
void OnCadDone(bool cadResult);

time_t cadTime;
time_t channelTimeout;
uint8_t channelFreeRetryNum = 0;

#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
/** Receiver buffer */
static uint8_t RcvBuffer[256];
#endif
/** Transmit buffer */
static uint8_t TxdBuffer[256];

int16_t lastRSSI = 0;

/** LoRa task handle */
TaskHandle_t loraTaskHandle;
/** Sensor reading task */
void loraTask(void *pvParameters);

/** Semaphore used by SX126x IRQ handler to wake up LoRaWan task */
SemaphoreHandle_t loraEvent = NULL;

/**
   @brief SX126x interrupt handler
   Called when DIO1 is set by SX126x
   Gives loraEvent semaphore to wake up LoRaWan handler task
*/
void loraIntHandler(void)
{
	// SX126x set IRQ
	if (loraEvent != NULL)
	{
		// Wake up LoRa task
		xSemaphoreGive(loraEvent);
	}
}

bool initLoRa(void)
{
	// Create the semaphore
	myLog_d("Create LoRa semaphore");
	delay(100); // Give Serial time to send
	loraEvent = xSemaphoreCreateBinary();

	// Give the semaphore, seems to be required to initialize it
	myLog_d("Initialize LoRa Semaphore");
	delay(100); // Give Serial time to send
	xSemaphoreGive(loraEvent);

	// Take the semaphore, so loop will be stopped waiting to get it
	myLog_d("Take LoRa Semaphore");
	delay(100); // Give Serial time to send
	xSemaphoreTake(loraEvent, 10);

	// Initialize library
	if (lora_rak4630_init() == 1)
	{
		return false;
	}

	// Initialize the Radio
	RadioEvents.TxDone = OnTxDone;
	RadioEvents.RxDone = OnRxDone;
	RadioEvents.TxTimeout = OnTxTimeout;
	RadioEvents.RxTimeout = OnRxTimeout;
	RadioEvents.RxError = OnRxError;
	RadioEvents.CadDone = OnCadDone;

	Radio.Init(&RadioEvents);

	Radio.Sleep(); // Radio.Standby();

	Radio.SetChannel(RF_FREQUENCY);

	Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
					  LORA_SPREADING_FACTOR, LORA_CODINGRATE,
					  LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
					  true, 0, 0, LORA_IQ_INVERSION_ON, TX_TIMEOUT_VALUE);

	Radio.SetRxConfig(MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
					  LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH,
					  LORA_SYMBOL_TIMEOUT, LORA_FIX_LENGTH_PAYLOAD_ON,
					  0, true, 0, 0, LORA_IQ_INVERSION_ON, true);

	// In deep sleep we need to hijack the SX126x IRQ to trigger a wakeup of the nRF52
	attachInterrupt(PIN_LORA_DIO_1, loraIntHandler, RISING);

	// Start the task that will handle the LoRaWan events
	myLog_d("Starting LoRaWan task");
	if (!xTaskCreate(loraTask, "LORA", 2048, NULL, TASK_PRIO_LOW, &loraTaskHandle))
	{
		return false;
	}

#ifdef TX_ONLY
	Radio.Sleep(); // Radio.Standby();
	// Radio.Sleep();
#else
	// To get maximum power savings we use Radio.SetRxDutyCycle instead of Radio.Rx(0)
	// This function keeps the SX1261/2 chip most of the time in sleep and only wakes up short times
	// to catch incoming data packages
	// See document SX1261_AN1200.36_SX1261-2_RxDutyCycle_V1.0 ==>> https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R0000001O3w/zsdHpRveb0_jlgJEedwalzsBaBnALfRq_MnJ25M_wtI
	Radio.SetRxDutyCycle(2 * 1024 * 1000 * 15.625, 10 * 1024 * 15.625);
#endif
	return true;
}

/**
   @brief Independent task to handle LoRa events
   @param pvParameters Unused
*/
void loraTask(void *pvParameters)
{
	while (1)
	{
		// Only if semaphore is available we need to handle LoRa events.
		// Otherwise we sleep here until an event occurs
		if (xSemaphoreTake(loraEvent, portMAX_DELAY) == pdTRUE)
		{
			// Switch on the indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
			digitalWrite(LED_CONN, HIGH);
#endif

			// Handle Radio events with special process command!!!!
			Radio.IrqProcessAfterDeepSleep();
		}
	}
}

/**
 * @brief Prepare packet to be sent and start CAD routine
 * 
 */
void sendLoRa(void)
{
	TxdBuffer[0] = 7;	 // Device ID
	TxdBuffer[1] = 0;	 // Lights status
	TxdBuffer[2] = 0;	 // Lights on/off
	TxdBuffer[3] = 27;	 // Temperature ones/tens/hundreds
	TxdBuffer[4] = 35;	 // Temperature tenths/hundredths
	TxdBuffer[5] = 67;	 // Humidity ones/tens/hundreds
	TxdBuffer[6] = 55;	 // Humidity tenths/hundredths
	TxdBuffer[7] = 34;	 // Light value
	TxdBuffer[8] = 12;	 // Light value
	TxdBuffer[9] = 75;	 // Light activation treshold
	TxdBuffer[10] = 00;	 // Light activation treshold
	TxdBuffer[11] = -80; // Strength of last received signal
	TxdBuffer[12] = 0;	 // Request date/time update
	TxdBuffer[13] = 0;	 // Flag for secondary light

	// Prepare LoRa CAD
	Radio.Sleep(); // Radio.Standby();
	Radio.SetCadParams(LORA_CAD_08_SYMBOL, LORA_SPREADING_FACTOR + 13, 10, LORA_CAD_ONLY, 0);
	cadTime = millis();
	channelTimeout = millis();

	// Switch on Indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
	digitalWrite(LED_CONN, HIGH);
#endif

	// Start CAD
	Radio.StartCad();

	// Send LoRa handler back to sleep
	xSemaphoreTake(loraEvent, 10);
}

/**
 * @brief Function to be executed on Radio Tx Done event
 */
void OnTxDone(void)
{
	myLog_d("OnTxDone");
#ifdef TX_ONLY
	Radio.Sleep(); // Radio.Standby();
	// Radio.Sleep();
#else
	// To get maximum power savings we use Radio.SetRxDutyCycle instead of Radio.Rx(0)
	// This function keeps the SX1261/2 chip most of the time in sleep and only wakes up short times
	// to catch incoming data packages
	// See document SX1261_AN1200.36_SX1261-2_RxDutyCycle_V1.0 ==>> https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R0000001O3w/zsdHpRveb0_jlgJEedwalzsBaBnALfRq_MnJ25M_wtI
	Radio.SetRxDutyCycle(2 * 1024 * 1000 * 15.625, 10 * 1024 * 15.625);
#endif

	// Switch off the indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
	digitalWrite(LED_CONN, LOW);
#endif

	// Send LoRa handler back to sleep
	xSemaphoreTake(loraEvent, 10);
}

/**@brief Function to be executed on Radio Rx Done event
 */
void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
	myLog_d("OnRxDone");

	delay(10);

	eventType = 0;
	// Notify task about the event
	if (taskEvent != NULL)
	{
		xSemaphoreGive(taskEvent);
	}

#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
	char rcvdData[256 * 4] = {0};

	int index = 0;
	for (int idx = 0; idx < (size * 3); idx += 3)
	{
		sprintf(&rcvdData[idx], "%02X ", payload[index++]);
	}
	myLog_d(rcvdData);
#endif

#ifdef TX_ONLY
	Radio.Sleep(); // Radio.Standby();
	// Radio.Sleep();
#else
	// To get maximum power savings we use Radio.SetRxDutyCycle instead of Radio.Rx(0)
	// This function keeps the SX1261/2 chip most of the time in sleep and only wakes up short times
	// to catch incoming data packages
	// See document SX1261_AN1200.36_SX1261-2_RxDutyCycle_V1.0 ==>> https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R0000001O3w/zsdHpRveb0_jlgJEedwalzsBaBnALfRq_MnJ25M_wtI
	Radio.SetRxDutyCycle(2 * 1024 * 1000 * 15.625, 10 * 1024 * 15.625);
#endif

	// Switch off the indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
	digitalWrite(LED_CONN, LOW);
#endif

	// Send LoRa handler back to sleep
	xSemaphoreTake(loraEvent, 10);
}

/**@brief Function to be executed on Radio Tx Timeout event
 */
void OnTxTimeout(void)
{
	myLog_d("OnTxTimeout");

#ifdef TX_ONLY
	Radio.Sleep(); // Radio.Standby();
	// Radio.Sleep();
#else
	// To get maximum power savings we use Radio.SetRxDutyCycle instead of Radio.Rx(0)
	// This function keeps the SX1261/2 chip most of the time in sleep and only wakes up short times
	// to catch incoming data packages
	// See document SX1261_AN1200.36_SX1261-2_RxDutyCycle_V1.0 ==>> https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R0000001O3w/zsdHpRveb0_jlgJEedwalzsBaBnALfRq_MnJ25M_wtI
	Radio.SetRxDutyCycle(2 * 1024 * 1000 * 15.625, 10 * 1024 * 15.625);
#endif

	// Switch off the indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
	digitalWrite(LED_CONN, LOW);
#endif

	// Send LoRa handler back to sleep
	xSemaphoreTake(loraEvent, 10);
}

/**@brief Function to be executed on Radio Rx Timeout event
 */
void OnRxTimeout(void)
{
	myLog_d("OnRxTimeout");

#ifdef TX_ONLY
	Radio.Sleep(); // Radio.Standby();
	// Radio.Sleep();
#else
	// To get maximum power savings we use Radio.SetRxDutyCycle instead of Radio.Rx(0)
	// This function keeps the SX1261/2 chip most of the time in sleep and only wakes up short times
	// to catch incoming data packages
	// See document SX1261_AN1200.36_SX1261-2_RxDutyCycle_V1.0 ==>> https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R0000001O3w/zsdHpRveb0_jlgJEedwalzsBaBnALfRq_MnJ25M_wtI
	Radio.SetRxDutyCycle(2 * 1024 * 1000 * 15.625, 10 * 1024 * 15.625);
#endif

	// Switch off the indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
	digitalWrite(LED_CONN, LOW);
#endif

	// Send LoRa handler back to sleep
	xSemaphoreTake(loraEvent, 10);
}

/**@brief Function to be executed on Radio Rx Error event
 */
void OnRxError(void)
{
#ifdef TX_ONLY
	Radio.Sleep(); // Radio.Standby();
	// Radio.Sleep();
#else
	// To get maximum power savings we use Radio.SetRxDutyCycle instead of Radio.Rx(0)
	// This function keeps the SX1261/2 chip most of the time in sleep and only wakes up short times
	// to catch incoming data packages
	// See document SX1261_AN1200.36_SX1261-2_RxDutyCycle_V1.0 ==>> https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R0000001O3w/zsdHpRveb0_jlgJEedwalzsBaBnALfRq_MnJ25M_wtI
	Radio.SetRxDutyCycle(2 * 1024 * 1000 * 15.625, 10 * 1024 * 15.625);
#endif

	// Switch off the indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
	digitalWrite(LED_CONN, LOW);
#endif

	// Send LoRa handler back to sleep
	xSemaphoreTake(loraEvent, 10);
}

/**@brief Function to be executed on Radio Rx Error event
 */
void OnCadDone(bool cadResult)
{
	if (cadResult)
	{
#ifdef TX_ONLY
		Radio.Sleep(); // Radio.Standby();
		// Radio.Sleep();
#else
		// To get maximum power savings we use Radio.SetRxDutyCycle instead of Radio.Rx(0)
		// This function keeps the SX1261/2 chip most of the time in sleep and only wakes up short times
		// to catch incoming data packages
		// See document SX1261_AN1200.36_SX1261-2_RxDutyCycle_V1.0 ==>> https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/2R0000001O3w/zsdHpRveb0_jlgJEedwalzsBaBnALfRq_MnJ25M_wtI
		Radio.SetRxDutyCycle(2 * 1024 * 1000 * 15.625, 10 * 1024 * 15.625);
#endif

		// Switch off the indicator lights
#if MYLOG_LOG_LEVEL > MYLOG_LOG_LEVEL_NONE
		digitalWrite(LED_CONN, LOW);
#endif

		// Send LoRa handler back to sleep
		xSemaphoreTake(loraEvent, 10);
	}
	else
	{
		myLog_d("CAD returned channel free after %ldms\n", (long)(millis() - cadTime));
		Radio.Send(TxdBuffer, 14);
	}
}
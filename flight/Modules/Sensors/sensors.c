/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Sensors
 * @brief Acquires sensor data 
 * Specifically updates the the @ref Gyros, @ref Accels, and @ref Magnetometer objects
 * @{
 *
 * @file       sensors.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref Gyros @ref Accels @ref Magnetometer
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "attitude.h"
#include "magnetometer.h"
#include "accels.h"
#include "gyros.h"
#include "gyrosbias.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "revocalibration.h"
#include "flightstatus.h"
#include "gpsposition.h"
#include "baroaltitude.h"
#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES 1540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)
#define SENSOR_PERIOD 2

#define F_PI 3.14159265358979323846f
#define PI_MOD(x) (fmod(x + F_PI, F_PI * 2) - F_PI)
// Private types

// Private variables
static xTaskHandle sensorsTaskHandle;
static bool gps_updated = false;
static bool baro_updated = false;

// Private functions
static void SensorsTask(void *parameters);
static void settingsUpdatedCb(UAVObjEvent * objEv);
static void sensorsUpdatedCb(UAVObjEvent * objEv);

// These values are initialized by settings but can be updated by the attitude algorithm
static bool bias_correct_gyro = true;

static float mag_bias[3] = {0,0,0};
static float mag_scale[3] = {0,0,0};
static float accel_bias[3] = {0,0,0};
static float accel_scale[3] = {0,0,0};

/**
 * API for sensor fusion algorithms:
 * Configure(xQueueHandle gyro, xQueueHandle accel, xQueueHandle mag, xQueueHandle baro)
 *   Stores all the queues the algorithm will pull data from
 * FinalizeSensors() -- before saving the sensors modifies them based on internal state (gyro bias)
 * Update() -- queries queues and updates the attitude estiamte
 */


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsInitialize(void)
{
	GyrosInitialize();
	GyrosBiasInitialize();
	AccelsInitialize();
	MagnetometerInitialize();
	RevoCalibrationInitialize();

	RevoCalibrationConnectCallback(&settingsUpdatedCb);
	return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsStart(void)
{
	// Start main task
	xTaskCreate(SensorsTask, (signed char *)"Sensors", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &sensorsTaskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_SENSORS, sensorsTaskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_SENSORS);

	return 0;
}

MODULE_INITCALL(SensorsInitialize, SensorsStart)

int32_t accel_test;
int32_t gyro_test;
int32_t mag_test;
//int32_t pressure_test;


/**
 * The sensor task.  This polls the gyros at 500 Hz and pumps that data to
 * stabilization and to the attitude loop
 * 
 * This function has a lot of if/defs right now to allow these configurations:
 * 1. BMA180 accel and MPU6000 gyro
 * 2. MPU6000 gyro and accel
 * 3. BMA180 accel and L3GD20 gyro
 */
static void SensorsTask(void *parameters)
{
	uint8_t init = 0;
	portTickType lastSysTime;
	uint32_t accel_samples;
	uint32_t gyro_samples;
	int32_t accel_accum[3] = {0, 0, 0};
	int32_t gyro_accum[3] = {0,0,0};
	float gyro_scaling;
	float accel_scaling;

	AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);

	UAVObjEvent ev;
	settingsUpdatedCb(&ev);

#if defined(PIOS_INCLUDE_MPU6000)
	gyro_test = PIOS_MPU6000_Test();
#if !defined(PIOS_INCLUDE_BMA180)
	accel_test = gyro_test;
#endif
#elif defined(PIOS_INCLUDE_L3GD20)
	gyro_test = PIOS_L3GD20_Test();
#endif
#if defined(PIOS_INCLUDE_BMA180)
	accel_test = PIOS_BMA180_Test();
#endif
	mag_test = PIOS_HMC5883_Test();
	
	if(accel_test < 0 || gyro_test < 0 || mag_test < 0) {
		AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
		while(1) {
			PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
			vTaskDelay(10);
		}
	}
	
	// If debugging connect callback
	if(pios_com_aux_id != 0) {
		BaroAltitudeConnectCallback(&sensorsUpdatedCb);
		GPSPositionConnectCallback(&sensorsUpdatedCb);
	}
	
	// Main task loop
	lastSysTime = xTaskGetTickCount();
	bool error = false;
	while (1) {
		// TODO: add timeouts to the sensor reads and set an error if the fail

		if (error) {
			PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
			lastSysTime = xTaskGetTickCount();
			vTaskDelayUntil(&lastSysTime, SENSOR_PERIOD / portTICK_RATE_MS);
			AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
			error = false;
		} else {
			AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);
		}

		int32_t read_good;
		int32_t count;

		for (int i = 0; i < 3; i++) {
			accel_accum[i] = 0;
			gyro_accum[i] = 0;
		}
		accel_samples = 0;
		gyro_samples = 0;

		// Make sure we get one sample
#if !defined(PIOS_MPU6000_ACCEL)
		struct pios_bma180_data accel;

		count = 0;
		while((read_good = PIOS_BMA180_ReadFifo(&accel)) != 0 && !error)
			error = ((xTaskGetTickCount() - lastSysTime) > SENSOR_PERIOD) ? true : error;
		if (error) {
			// Unfortunately if the BMA180 ever misses getting read, then it will not
			// trigger more interrupts.  In this case we must force a read to kickstarts
			// it.
			struct pios_bma180_data data;
			PIOS_BMA180_ReadAccels(&data);
			continue;
		}
		while(read_good == 0) {	
			count++;
			
			accel_accum[0] += accel.x;
			accel_accum[1] += accel.y;
			accel_accum[2] += accel.z;
			
			read_good = PIOS_BMA180_ReadFifo(&accel);
		}
		accel_samples = count;
		accel_scaling = PIOS_BMA180_GetScale();
#endif

// Using MPU6000 gyro and possibly accel
#if defined(PIOS_INCLUDE_MPU6000)
		struct pios_mpu6000_data gyro;

		count = 0;
		while((read_good = PIOS_MPU6000_ReadFifo(&gyro)) != 0 && !error)
			error = ((xTaskGetTickCount() - lastSysTime) > SENSOR_PERIOD) ? true : error;
		if (error)
			continue;
		while(read_good == 0) {
			count++;
			
			gyro_accum[0] += gyro.gyro_x;
			gyro_accum[1] += gyro.gyro_y;
			gyro_accum[2] += gyro.gyro_z;
			
#if defined(PIOS_MPU6000_ACCEL)
			accel_accum[0] += gyro.accel_x;
			accel_accum[1] += gyro.accel_y;
			accel_accum[2] += gyro.accel_z;
#endif

			read_good = PIOS_MPU6000_ReadFifo(&gyro);
		}
		gyro_samples = count;
		gyro_scaling = PIOS_MPU6000_GetScale();

#if defined(PIOS_MPU6000_ACCEL)
		accel_samples = count;
		accel_scaling = PIOS_MPU6000_GetAccelScale();
#endif

// Using L3DG20 gyro
#elif defined(PIOS_INCLUDE_L3GD20)
		struct pios_l3gd20_data gyro;
		gyro_samples = 0;
		xQueueHandle gyro_queue = PIOS_L3GD20_GetQueue();
		
		while(xQueueReceive(gyro_queue, (void *) &gyro, 0) != errQUEUE_EMPTY) {
			gyro_samples++;
			gyro_accum[0] += gyro.gyro_x;
			gyro_accum[1] += gyro.gyro_y;
			gyro_accum[2] += gyro.gyro_z;
		}
		gyro_scaling = PIOS_L3GD20_GetScale();

#else
//#error No gyro defined
		struct gyro_data {float x; float y; float z; float temperature;} gyro;
		gyro_scaling = 0;
		gyro_samples = 1;
#endif
		float accels[3] = {(float) accel_accum[1] / accel_samples, (float) accel_accum[0] / accel_samples, -(float) accel_accum[2] / accel_samples};

		// Not the swaping of channel orders
#if defined(PIOS_MPU6000_ACCEL)
		accel_scaling = PIOS_MPU6000_GetAccelScale();
#else
		accel_scaling = PIOS_BMA180_GetScale();
#endif
		AccelsData accelsData; // Skip get as we set all the fields
		accelsData.x = accels[0] * accel_scaling * accel_scale[0] - accel_bias[0];
		accelsData.y = accels[1] * accel_scaling * accel_scale[1] - accel_bias[1];
		accelsData.z = accels[2] * accel_scaling * accel_scale[2] - accel_bias[2];
#if defined(BMA180)
		accelsData.temperature = 25.0f + ((float) accel.temperature - 2.0f) / 2.0f;
#elif defined(PIOS_MPU6000_ACCEL)
		accelsData.temperature = 35.0f + ((float) gyro.temperature + 512.0f) / 340.0f;
#endif
		accelsData.temperature = 
		AccelsSet(&accelsData);

		float gyros[3] = {(float) gyro_accum[1] / gyro_samples, (float) gyro_accum[0] / gyro_samples, -(float) gyro_accum[2] / gyro_samples};

		GyrosData gyrosData; // Skip get as we set all the fields
		gyrosData.x = gyros[0] * gyro_scaling;
		gyrosData.y = gyros[1] * gyro_scaling;
		gyrosData.z = gyros[2] * gyro_scaling;
#if defined(PIOS_INCLUDE_MPU6000)
		gyrosData.temperature = 35.0f + ((float) gyro.temperature + 512.0f) / 340.0f;
#else
		gyrosData.temperature = gyro.temperature;
#endif
		if (bias_correct_gyro) {
			// Apply bias correction to the gyros
			GyrosBiasData gyrosBias;
			GyrosBiasGet(&gyrosBias);
			gyrosData.x += gyrosBias.x;
			gyrosData.y += gyrosBias.y;
			gyrosData.z += gyrosBias.z;
		}
		
		GyrosSet(&gyrosData);
		
		// Because most crafts wont get enough information from gravity to zero yaw gyro, we try
		// and make it average zero (weakly)
		MagnetometerData mag;
		bool mag_updated = false;
		if (PIOS_HMC5883_NewDataAvailable()) {
			mag_updated = true;
			int16_t values[3];
			PIOS_HMC5883_ReadMag(values);
			mag.x = values[1] * mag_scale[0] - mag_bias[0];
			mag.y = values[0] * mag_scale[1] - mag_bias[1];
			mag.z = -values[2] * mag_scale[2] - mag_bias[2];
			MagnetometerSet(&mag);
		}

		// For debugging purposes here we can output all of the sensors.  Do it as a single transaction
		// so the message isn't split if anything else is writing to it
		if(pios_com_aux_id != 0) {
			uint32_t message_size = 3;
			uint8_t message[200] = {0xff, (lastSysTime & 0xff00) >> 8, lastSysTime & 0x00ff};
			
			// Add accel data
			memcpy(&message[message_size], &accelsData.x, sizeof(accelsData.x) * 3);
			message_size += sizeof(accelsData.x) * 3;
			
			// Add gyro data with temp
			memcpy(&message[message_size], &gyrosData, sizeof(gyrosData));
			message_size += sizeof(gyrosData);
			
			if(mag_updated) { // Add mag data
				message[message_size] = 0x01; // Indicate mag data here
				message_size++;
				memcpy(&message[message_size], &mag, sizeof(mag));
				message_size += sizeof(mag);
			}
			
			if(gps_updated) { // Add GPS data
				gps_updated = false;
				GPSPositionData gps;
				GPSPositionGet(&gps);
				message[message_size] = 0x02; // Indicate gps data here
				message_size++;
				memcpy(&message[message_size], &gps, sizeof(gps));
				message_size += sizeof(gps);
			}
			
			if(baro_updated) { // Add baro data
				baro_updated = false;
				BaroAltitudeData baro;
				BaroAltitudeGet(&baro);
				message[message_size] = 0x03; // Indicate mag data here
				message_size++;
				memcpy(&message[message_size], &baro, sizeof(baro));
				message_size += sizeof(baro);
			}
			
			PIOS_COM_SendBufferNonBlocking(pios_com_aux_id, message, message_size);

		}
		
		PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);

		// For L3GD20 which runs at 760 then one cycle per sample
#if defined(PIOS_INCLUDE_MPU6000) && !defined(PIOS_INCLUDE_L3GD20)
		vTaskDelayUntil(&lastSysTime, SENSOR_PERIOD / portTICK_RATE_MS);
#endif
	}
}

/**
 * Indicate that these sensors have been updated
 */
static void sensorsUpdatedCb(UAVObjEvent * objEv)
{
	if(objEv->obj == GPSPositionHandle())
		gps_updated = true;
	if(objEv->obj == BaroAltitudeHandle())
		baro_updated = true;
}

/**
 * Locally cache some variables from the AtttitudeSettings object
 */
static void settingsUpdatedCb(UAVObjEvent * objEv) {
	RevoCalibrationData cal;
	RevoCalibrationGet(&cal);
	
	mag_bias[0] = cal.mag_bias[REVOCALIBRATION_MAG_BIAS_X];
	mag_bias[1] = cal.mag_bias[REVOCALIBRATION_MAG_BIAS_Y];
	mag_bias[2] = cal.mag_bias[REVOCALIBRATION_MAG_BIAS_Z];
	mag_scale[0] = cal.mag_scale[REVOCALIBRATION_MAG_SCALE_X];
	mag_scale[1] = cal.mag_scale[REVOCALIBRATION_MAG_SCALE_Y];
	mag_scale[2] = cal.mag_scale[REVOCALIBRATION_MAG_SCALE_Z];
	accel_bias[0] = cal.accel_bias[REVOCALIBRATION_ACCEL_BIAS_X];
	accel_bias[1] = cal.accel_bias[REVOCALIBRATION_ACCEL_BIAS_Y];
	accel_bias[2] = cal.accel_bias[REVOCALIBRATION_ACCEL_BIAS_Z];
	accel_scale[0] = cal.accel_scale[REVOCALIBRATION_ACCEL_SCALE_X];
	accel_scale[1] = cal.accel_scale[REVOCALIBRATION_ACCEL_SCALE_Y];
	accel_scale[2] = cal.accel_scale[REVOCALIBRATION_ACCEL_SCALE_Z];
}
/**
  * @}
  * @}
  */

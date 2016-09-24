/*
 * This file is part of RaceFlight.
 *
 * RaceFlight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RaceFlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RaceFlight.  If not, see <http://www.gnu.org/licenses/>.
 */



#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "common/maths.h"

#include "build_config.h"
#include "platform.h"
#include "debug.h"

#include "common/axis.h"
#include "common/filter.h"

#include "drivers/system.h"
#include "drivers/sensor.h"
#include "drivers/accgyro.h"
#include "drivers/compass.h"

#include "sensors/sensors.h"
#include "sensors/gyro.h"
#include "sensors/compass.h"
#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/sonar.h"

#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/imu.h"

#include "io/gps.h"

#include "config/runtime_config.h"





#define SPIN_RATE_LIMIT 20

int16_t accSmooth[XYZ_AXIS_COUNT];
int32_t accSum[XYZ_AXIS_COUNT];

uint32_t accTimeSum = 0;        
int accSumCount = 0;
float accVelScale;

float throttleAngleScale;
float fc_acc;
float smallAngleCosZ = 0;

float magneticDeclination = 0.0f;       
static bool isAccelUpdatedAtLeastOnce = false;

static imuRuntimeConfig_t *imuRuntimeConfig;
static pidProfile_t *pidProfile;
static accDeadband_t *accDeadband;

STATIC_UNIT_TESTED float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;    
static float rMat[3][3];

attitudeEulerAngles_t attitude = { { 0, 0, 0 } };     

static float gyroScale;

STATIC_UNIT_TESTED void imuComputeRotationMatrix(void)
{
    float q1q1 = sq(q1);
    float q2q2 = sq(q2);
    float q3q3 = sq(q3);
    
    float q0q1 = q0 * q1;
    float q0q2 = q0 * q2;
    float q0q3 = q0 * q3;
    float q1q2 = q1 * q2;
    float q1q3 = q1 * q3;
    float q2q3 = q2 * q3;

    rMat[0][0] = 1.0f - 2.0f * q2q2 - 2.0f * q3q3;
    rMat[0][1] = 2.0f * (q1q2 + -q0q3);
    rMat[0][2] = 2.0f * (q1q3 - -q0q2);

    rMat[1][0] = 2.0f * (q1q2 - -q0q3);
    rMat[1][1] = 1.0f - 2.0f * q1q1 - 2.0f * q3q3;
    rMat[1][2] = 2.0f * (q2q3 + -q0q1);

    rMat[2][0] = 2.0f * (q1q3 + -q0q2);
    rMat[2][1] = 2.0f * (q2q3 - -q0q1);
    rMat[2][2] = 1.0f - 2.0f * q1q1 - 2.0f * q2q2;
}

void imuConfigure(
    imuRuntimeConfig_t *initialImuRuntimeConfig,
    pidProfile_t *initialPidProfile,
    accDeadband_t *initialAccDeadband,
    float accz_lpf_cutoff,
    uint16_t throttle_correction_angle
)
{
    imuRuntimeConfig = initialImuRuntimeConfig;
    pidProfile = initialPidProfile;
    accDeadband = initialAccDeadband;
    fc_acc = calculateAccZLowPassFilterRCTimeConstant(accz_lpf_cutoff);
    throttleAngleScale = calculateThrottleAngleScale(throttle_correction_angle);
}

void imuInit(void)
{
    smallAngleCosZ = cos_approx(degreesToRadians(imuRuntimeConfig->small_angle));
    gyroScale = gyro.scale * (M_PIf / 180.0f);  
    accVelScale = 9.80665f / acc_1G / 10000.0f;

    imuComputeRotationMatrix();
}

float calculateThrottleAngleScale(uint16_t throttle_correction_angle)
{
    return (1800.0f / M_PIf) * (900.0f / throttle_correction_angle);
}

/*
* Calculate RC time constant used in the accZ lpf.
*/
float calculateAccZLowPassFilterRCTimeConstant(float accz_lpf_cutoff)
{
    return 0.5f / (M_PIf * accz_lpf_cutoff);
}

void imuResetAccelerationSum(void)
{
    accSum[0] = 0;
    accSum[1] = 0;
    accSum[2] = 0;
    accSumCount = 0;
    accTimeSum = 0;
}

void imuTransformVectorBodyToEarth(t_fp_vector * v)
{
    float x,y,z;

    /* From body frame to earth frame */
    x = rMat[0][0] * v->V.X + rMat[0][1] * v->V.Y + rMat[0][2] * v->V.Z;
    y = rMat[1][0] * v->V.X + rMat[1][1] * v->V.Y + rMat[1][2] * v->V.Z;
    z = rMat[2][0] * v->V.X + rMat[2][1] * v->V.Y + rMat[2][2] * v->V.Z;

    v->V.X = x;
    v->V.Y = -y;
    v->V.Z = z;
}


void imuCalculateAcceleration(uint32_t deltaT)
{
    static int32_t accZoffset = 0;
    static float accz_smooth = 0;
    float dT;
    t_fp_vector accel_ned;

    
    dT = (float)deltaT * 1e-6f;

    accel_ned.V.X = accSmooth[0];
    accel_ned.V.Y = accSmooth[1];
    accel_ned.V.Z = accSmooth[2];

    imuTransformVectorBodyToEarth(&accel_ned);

    if (imuRuntimeConfig->acc_unarmedcal == 1) {
        if (!ARMING_FLAG(ARMED)) {
            accZoffset -= accZoffset / 64;
            accZoffset += accel_ned.V.Z;
        }
        accel_ned.V.Z -= accZoffset / 64;  
    } else
        accel_ned.V.Z -= acc_1G;

    accz_smooth = accz_smooth + (dT / (fc_acc + dT)) * (accel_ned.V.Z - accz_smooth); 

    
    accSum[X] += applyDeadband((int32_t)(accel_ned.V.X), accDeadband->xy);
	accSum[Y] += applyDeadband((int32_t)(accel_ned.V.Y), accDeadband->xy);
	accSum[Z] += applyDeadband((int32_t)(accz_smooth), accDeadband->z);

    
    accTimeSum += deltaT;
    accSumCount++;
}

static float invSqrt(float x)
{
    return 1.0f / sqrtf(x);
}

static bool imuUseFastGains(void)
{
    return !ARMING_FLAG(ARMED) && millis() < 20000;
}

static float imuGetPGainScaleFactor(void)
{
    if (imuUseFastGains()) {
        return 10.0f;
    }
    else {
        return 1.0f;
    }
}

static void imuMahonyAHRSupdate(float dt, float gx, float gy, float gz,
                                bool useAcc, float ax, float ay, float az,
                                bool useMag, float mx, float my, float mz,
                                bool useYaw, float yawError)
{
    static float integralFBx = 0.0f,  integralFBy = 0.0f, integralFBz = 0.0f;    
    float recipNorm;
    float hx, hy, bx;
    float ex = 0, ey = 0, ez = 0;
    float qa, qb, qc;

    
    float spin_rate = sqrtf(sq(gx) + sq(gy) + sq(gz));

    
    if (useYaw) {
        while (yawError >  M_PIf) yawError -= (2.0f * M_PIf);
        while (yawError < -M_PIf) yawError += (2.0f * M_PIf);

        ez += sin_approx(yawError / 2.0f);
    }

    
    recipNorm = sq(mx) + sq(my) + sq(mz);
    if (useMag && recipNorm > 0.01f) {
        
        recipNorm = invSqrt(recipNorm);
        mx *= recipNorm;
        my *= recipNorm;
        mz *= recipNorm;

        
        

        
        
        hx = rMat[0][0] * mx + rMat[0][1] * my + rMat[0][2] * mz;
        hy = rMat[1][0] * mx + rMat[1][1] * my + rMat[1][2] * mz;
        bx = sqrtf(hx * hx + hy * hy);

        
        float ez_ef = -(hy * bx);

        
        ex += rMat[2][0] * ez_ef;
        ey += rMat[2][1] * ez_ef;
        ez += rMat[2][2] * ez_ef;
    }

    
    recipNorm = sq(ax) + sq(ay) + sq(az);
    if (useAcc && recipNorm > 0.01f) {
        
        recipNorm = invSqrt(recipNorm);
        ax *= recipNorm;
        ay *= recipNorm;
        az *= recipNorm;

        
        ex += (ay * rMat[2][2] - az * rMat[2][1]);
        ey += (az * rMat[2][0] - ax * rMat[2][2]);
        ez += (ax * rMat[2][1] - ay * rMat[2][0]);
    }

    
    if(imuRuntimeConfig->dcm_ki > 0.0f) {
        
        if (spin_rate < DEGREES_TO_RADIANS(SPIN_RATE_LIMIT)) {
            float dcmKiGain = imuRuntimeConfig->dcm_ki;
            integralFBx += dcmKiGain * ex * dt;    
            integralFBy += dcmKiGain * ey * dt;
            integralFBz += dcmKiGain * ez * dt;
        }
    }
    else {
        integralFBx = 0.0f;    
        integralFBy = 0.0f;
        integralFBz = 0.0f;
    }

    
    float dcmKpGain = imuRuntimeConfig->dcm_kp * imuGetPGainScaleFactor();

    
    gx += dcmKpGain * ex + integralFBx;
    gy += dcmKpGain * ey + integralFBy;
    gz += dcmKpGain * ez + integralFBz;

    
    gx *= (0.5f * dt);
    gy *= (0.5f * dt);
    gz *= (0.5f * dt);

    qa = q0;
    qb = q1;
    qc = q2;
    q0 += (-qb * gx - qc * gy - q3 * gz);
    q1 += (qa * gx + qc * gz - q3 * gy);
    q2 += (qa * gy - qb * gz + q3 * gx);
    q3 += (qa * gz + qb * gy - qc * gx);

    
    recipNorm = invSqrt(sq(q0) + sq(q1) + sq(q2) + sq(q3));
    q0 *= recipNorm;
    q1 *= recipNorm;
    q2 *= recipNorm;
    q3 *= recipNorm;

    
    imuComputeRotationMatrix();
}

STATIC_UNIT_TESTED void imuUpdateEulerAngles(void)
{
    /* Compute pitch/roll angles */
	attitude.values.roll = (int16_t)(atan2f(rMat[2][1], rMat[2][2]) * (1800.0f / M_PIf));
	attitude.values.pitch = (int16_t)(((0.5f * M_PIf) - acosf(-rMat[2][0])) * (1800.0f / M_PIf));
	attitude.values.yaw = (int16_t)((-atan2f(rMat[1][0], rMat[0][0]) * (1800.0f / M_PIf) + magneticDeclination));

    if (attitude.values.yaw < 0)
        attitude.values.yaw += 3600;

    /* Update small angle state */
    if (rMat[2][2] > smallAngleCosZ) {
        ENABLE_STATE(SMALL_ANGLE);
    } else {
        DISABLE_STATE(SMALL_ANGLE);
    }
}

static bool imuIsAccelerometerHealthy(void)
{
    int32_t axis;
    int32_t accMagnitude = 0;

    for (axis = 0; axis < 3; axis++) {
        accMagnitude += (int32_t)accSmooth[axis] * accSmooth[axis];
    }

    accMagnitude = accMagnitude * 100 / (sq((int32_t)acc_1G));

    return true; 
    
    return (72 < accMagnitude) && (accMagnitude < 133);
}

static bool isMagnetometerHealthy(void)
{
    return (magADC[X] != 0) && (magADC[Y] != 0) && (magADC[Z] != 0);
}

static void imuCalculateEstimatedAttitude(void)
{
	static biquad_t accLPFState[3];
	static bool accStateIsSet;
    static uint32_t previousIMUUpdateTime;
    float rawYawError = 0;
    int32_t axis;
    bool useAcc = false;
    bool useMag = false;
    bool useYaw = false;

    uint32_t currentTime = micros();
    uint32_t deltaT = currentTime - previousIMUUpdateTime;
    previousIMUUpdateTime = currentTime;

    
    for (axis = 0; axis < 3; axis++) {
        if (imuRuntimeConfig->acc_cut_hz) {
            if (accStateIsSet) {
                accSmooth[axis] = (int16_t)(applyBiQuadFilter((float) accADC[axis], &accLPFState[axis]));
            } else {
                for (axis = 0; axis < 3; axis++) BiQuadNewLpf((uint16_t)imuRuntimeConfig->acc_cut_hz, &accLPFState[axis], 1000);
                accStateIsSet = true;
            }
        } else {
            accSmooth[axis] = accADC[axis];
        }
    }

    if (imuIsAccelerometerHealthy()) {
        useAcc = true;
    }

    if (sensors(SENSOR_MAG) && isMagnetometerHealthy()) {
        useMag = true;
    }
#if defined(GPS)
    else if (STATE(FIXED_WING) && sensors(SENSOR_GPS) && STATE(GPS_FIX) && GPS_numSat >= 5 && GPS_speed >= 300) {
        
        rawYawError = DECIDEGREES_TO_RADIANS(attitude.values.yaw - GPS_ground_course);
        useYaw = true;
    }
#endif

    imuMahonyAHRSupdate(deltaT * 1e-6f,
                        gyroADC[X] * gyroScale, gyroADC[Y] * gyroScale, gyroADC[Z] * gyroScale,
                        useAcc, accSmooth[X], accSmooth[Y], accSmooth[Z],
                        useMag, magADC[X], magADC[Y], magADC[Z],
                        useYaw, rawYawError);

    imuUpdateEulerAngles();

    imuCalculateAcceleration(deltaT); 
}

void imuUpdateAccelerometer(rollAndPitchTrims_t *accelerometerTrims)
{
    if (sensors(SENSOR_ACC)) {
        updateAccelerationReadings(accelerometerTrims);
        isAccelUpdatedAtLeastOnce = true;
    }
}

void imuUpdateGyroAndAttitude(void)
{
    gyroUpdate();

    if (sensors(SENSOR_ACC) && isAccelUpdatedAtLeastOnce) {
        imuCalculateEstimatedAttitude();
    } else {
        accADC[X] = 0;
        accADC[Y] = 0;
        accADC[Z] = 0;
    }
}

float getCosTiltAngle(void)
{
    return rMat[2][2];
}

int16_t calculateThrottleAngleCorrection(uint8_t throttle_correction_value)
{
    /*
    * Use 0 as the throttle angle correction if we are inverted, vertical or with a
    * small angle < 0.86 deg
    * TODO: Define this small angle in config.
    */
    if (rMat[2][2] <= 0.015f) {
        return 0;
    }
    int angle = (int)(acosf(rMat[2][2]) * throttleAngleScale);
    if (angle > 900)
        angle = 900;
	return (int16_t)(throttle_correction_value * sin_approx(angle / (900.0f * M_PIf / 2.0f)));
}
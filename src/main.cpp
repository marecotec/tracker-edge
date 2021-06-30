/*
 * Copyright (c) 2020 Particle Industries, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Particle.h"

#include "tracker_config.h"
#include "tracker.h"
#include "bmi160.h"

SYSTEM_THREAD(ENABLED);
SYSTEM_MODE(SEMI_AUTOMATIC);

PRODUCT_ID(TRACKER_PRODUCT_ID);
PRODUCT_VERSION(TRACKER_PRODUCT_VERSION);

STARTUP(
    Tracker::startup();
);

SerialLogHandler logHandler(115200, LOG_LEVEL_TRACE, {
    { "app.gps.nmea", LOG_LEVEL_INFO },
    { "app.gps.ubx",  LOG_LEVEL_INFO },
    { "ncp.at", LOG_LEVEL_INFO },
    { "net.ppp.client", LOG_LEVEL_INFO },
});

void locationGenerationCallback(JSONWriter &writer, LocationPoint &point, const void *context); // Forward declaration

void setup()
{
    Tracker::instance().init();
    Tracker::instance().location.regLocGenCallback(locationGenerationCallback);

    SystemPowerConfiguration conf;
    conf.powerSourceMaxCurrent(500);
    System.setPowerConfiguration(conf);

    Particle.connect();
}

void loop()
{
    Tracker::instance().loop();

}


void locationGenerationCallback(JSONWriter &writer, LocationPoint &point, const void *context)
{
    Bmi160Accelerometer data;

    PMIC power(true);
    
    int batteryState = System.batteryState();
    float batterySoc = System.batteryCharge();

    constexpr char const* batteryStates[] = {
    "unknown", "not charging", "charging", "charged", "discharging", "fault", "disconnected"
    };

    int ret = BMI160.getAccelerometer(data);

    if (ret == SYSTEM_ERROR_NONE) {
        writer.name("x_accel").value(data.x, 3);
        writer.name("y_accel").value(data.y, 3);
        writer.name("z_accel").value(data.z, 3);
       
    }
    writer.name("v_acc").value(point.verticalAccuracy, 2);
    writer.name("VIN_Vmin").value(power.getInputVoltageLimit(), 2);
    writer.name("VIN_Imax").value(power.getInputCurrentLimit(), 2);
    writer.name("Ichg").value(power.getChargeCurrentValue(), 2);
    writer.name("Iterm").value(power.getChargeVoltageValue(), 2);
    writer.name("Power_Source").value(batteryStates[std::max(0,batteryState)]);
    writer.name("Battery_State").value(batteryStates[std::max(0, batteryState)]);
    writer.name("Battery_Charge").value(batterySoc);


}


 
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

#include <stdint.h>
#include <algorithm>

#include "Particle.h"
#include "tracker_config.h"
#include "tracker_location.h"

#include "config_service.h"
#include "location_service.h"

TrackerLocation *TrackerLocation::_instance = nullptr;

static constexpr system_tick_t LoopSampleRate = 1000; // milliseconds
static constexpr uint32_t EarlySleepSec = 2; // seconds
static constexpr uint32_t MiscSleepWakeSec = 3; // seconds - miscellaneous time spent by system entering and exiting sleep
static constexpr uint32_t LockTimeoutSec = 10; // seconds - time to wait for GNSS lock (sleep disabled)

static constexpr size_t EnhancedLocationQueueSize = 5; // up to this many elements
static constexpr size_t ObjectEstimateWpsHeaderSize = sizeof(",{\"wps\":[]}") - 1 /* null */;
static constexpr size_t ObjectEstimateWpsDataSize = sizeof("{\"bssid\":\"00:11:22:33:44:55\",\"ch\":99,\"str\":-999},") - 1 /* null */;

static int set_radius_cb(double value, const void *context)
{
    static_cast<LocationService *>((void *)context)->setRadiusThreshold(value);

    return 0;
}

static int get_radius_cb(double &value, const void *context)
{
    float temp;

    static_cast<LocationService *>((void *)context)->getRadiusThreshold(temp);

    value = temp;

    return 0;
}

// when entering the location config object copy from the actual config to the
// shadow for access if writing
int TrackerLocation::enter_location_config_cb(bool write, const void *context)
{
    if(write)
    {
        memcpy(&_config_state_shadow, &_config_state, sizeof(_config_state_shadow));
    }
    return 0;
}

// when exiting the location config object copy from the shadow config to the
// actual if writing (and no error)
int TrackerLocation::exit_location_config_cb(bool write, int status, const void *context)
{
    if(write && !status)
    {
        if(_config_state_shadow.interval_min_seconds > _config_state_shadow.interval_max_seconds)
        {
            return -EINVAL;
        }
        memcpy(&_config_state, &_config_state_shadow, sizeof(_config_state));
    }
    return status;
}

int TrackerLocation::get_loc_cb(CloudServiceStatus status,
    JSONValue *root,
    const void *context)
{
   triggerLocPub(Trigger::IMMEDIATE);
   return 0;
}

void TrackerLocation::init()
{
    static ConfigObject location_desc
    (
        "location",
        {
            ConfigFloat("radius", get_radius_cb, set_radius_cb, &LocationService::instance()).min(0.0).max(1000000.0),
            ConfigInt("interval_min", config_get_int32_cb, config_set_int32_cb,
                &_config_state.interval_min_seconds, &_config_state_shadow.interval_min_seconds,
                0, 86400l),
            ConfigInt("interval_max", config_get_int32_cb, config_set_int32_cb,
                &_config_state.interval_max_seconds, &_config_state_shadow.interval_max_seconds,
                0, 86400l),
            ConfigBool("min_publish",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.min_publish, &_config_state_shadow.min_publish),
            ConfigBool("lock_trigger",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.lock_trigger, &_config_state_shadow.lock_trigger),
            ConfigBool("loc_ack",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.process_ack, &_config_state_shadow.process_ack),
            ConfigBool("tower",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.tower, &_config_state_shadow.tower
            ),
            ConfigBool("gnss",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.gnss, &_config_state_shadow.gnss
            ),
            ConfigBool("wps",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.wps, &_config_state_shadow.wps
            ),
            ConfigBool("enhance_loc",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.enhance_loc, &_config_state_shadow.enhance_loc
            ),
            ConfigBool("loc_cb",
                config_get_bool_cb, config_set_bool_cb,
                &_config_state.loc_cb, &_config_state_shadow.loc_cb
            ),
        },
        std::bind(&TrackerLocation::enter_location_config_cb, this, _1, _2),
        std::bind(&TrackerLocation::exit_location_config_cb, this, _1, _2, _3)
    );

    ConfigService::instance().registerModule(location_desc);

    CloudService::instance().regCommandCallback("get_loc", &TrackerLocation::get_loc_cb, this);

    _last_location_publish_sec = System.uptime() - _config_state.interval_min_seconds;

    _sleep.registerSleepPrepare([this](TrackerSleepContext context){ this->onSleepPrepare(context); });
    _sleep.registerSleep([this](TrackerSleepContext context){ this->onSleep(context); });
    _sleep.registerSleepCancel([this](TrackerSleepContext context){ this->onSleepCancel(context); });
    _sleep.registerWake([this](TrackerSleepContext context){ this->onWake(context); });
    _sleep.registerStateChange([this](TrackerSleepContext context){ this->onSleepState(context); });

    CloudService::instance().regCommandCallback("loc-enhanced", &TrackerLocation::enhanced_cb, this);
}

int TrackerLocation::buildEnhLocation(JSONValue& node, LocationPoint& point) {
    JSONObjectIterator locChild(node);

    while(locChild.next()) {
        if (locChild.name() == "lat") {
            if (!locChild.value().isNumber()) {
                return -EINVAL;
            }
            point.latitude = (float)locChild.value().toDouble();
        }
        else if (locChild.name() == "lon") {
            if (!locChild.value().isNumber()) {
                return -EINVAL;
            }
            point.longitude = (float)locChild.value().toDouble();
        }
        else if (locChild.name() == "h_acc") {
            if (!locChild.value().isNumber()) {
                return -EINVAL;
            }
            point.horizontalAccuracy = (float)locChild.value().toDouble();
        }
        else if (locChild.name() == "src") {
            if (!locChild.value().isArray()) {
                return -EINVAL;
            }
            JSONArrayIterator srcList(locChild.value());
            while (srcList.next()) {
                if (srcList.value().isString()) {
                    auto source = LocationSource::NONE;
                    if (srcList.value().toString() == "cell") {
                        source = LocationSource::CELL;
                    }
                    else if (srcList.value().toString() == "wifi") {
                        source = LocationSource::WIFI;
                    }
                    else if (srcList.value().toString() == "gnss") {
                        source = LocationSource::GNSS;
                    }
                    else {
                        continue;
                    }
                    point.sources.append(source);
                }
                else {
                    return -EINVAL;
                }
            }
        }
    }

    return 0;
}

int TrackerLocation::enhanced_cb(CloudServiceStatus status, JSONValue *root, const void *context) {
    (void)status;
    (void)context;

    LocationPoint point = {};
    JSONValue* locObject = nullptr;
    JSONValue child;

    JSONObjectIterator item(*root);
    while(item.next()) {
        if ((item.name() == "loc-enhanced") && item.value().isObject()) {
            child = item.value();
            locObject = &child;
        }
    }

    if (locObject) {
        point.type = LocationType::CLOUD;
        buildEnhLocation(*locObject, point);
        for (auto item : enhancedLocCallbacks) {
            item(point);
        }
    }

    return 0;
}

int TrackerLocation::regLocGenCallback(
    std::function<void(JSONWriter&, LocationPoint &, const void *)> cb,
    const void *context)
{
    locGenCallbacks.append(std::bind(cb, _1, _2, context));
    return 0;
}

// register for callback on location publish success/fail
// these callbacks are NOT persistent and are used for the next publish
int TrackerLocation::regLocPubCallback(
    cloud_service_send_cb_t cb,
    const void *context)
{
    locPubCallbacks.append(std::bind(cb, _1, _2, _3, context));
    return 0;
}

int TrackerLocation::regEnhancedLocCallback(
    std::function<void(const LocationPoint &, const void *)> cb,
    const void *context)
{
    enhancedLocCallbacks.append(std::bind(cb, _1, context));
    return 0;
}

int TrackerLocation::triggerLocPub(Trigger type, const char *s)
{
    std::lock_guard<RecursiveMutex> lg(mutex);
    bool matched = false;

    for(auto trigger : _pending_triggers)
    {
        if(!strcmp(trigger, s))
        {
            matched = true;
            break;
        }
    }

    if(!matched)
    {
        _pending_triggers.append(s);
    }

    if(type == Trigger::IMMEDIATE)
    {
        _pending_immediate = true;
    }

    return 0;
}

void TrackerLocation::issue_location_publish_callbacks(CloudServiceStatus status, JSONValue *rsp_root, const char *req_event)
{
    for(auto cb : pendingLocPubCallbacks)
    {
        cb(status, rsp_root, req_event);
    }
    pendingLocPubCallbacks.clear();
}

int TrackerLocation::location_publish_cb(CloudServiceStatus status, JSONValue *rsp_root, const char *req_event, const void *context)
{
    bool issue_callbacks = true;

    if(status == CloudServiceStatus::SUCCESS)
    {
        // this could either be on the Particle Cloud ack (default) OR the
        // end-to-end ACK
        Log.info("location cb publish %lu success!", *(uint32_t *) context);
        _first_publish = false;
        _pending_first_publish = false;
    }
    else if(status == CloudServiceStatus::FAILURE)
    {
        // right now FAILURE only comes out of a Particle Cloud issue
        // once Particle Cloud passes if waiting on end-to-end it will
        // only ever timeout

        // save on failure for retry
        if(req_event && !location_publish_retry_str)
        {
            size_t len = strlen(req_event) + 1;
            location_publish_retry_str = (char *) malloc(len);
            if(location_publish_retry_str)
            {
                memcpy(location_publish_retry_str, req_event, len);
                // we've saved for retry, defer callbacks until retry completes
                issue_callbacks = false;
            }
        }
        Log.info("location cb publish %lu failure", *(uint32_t *) context);
    }
    else if(status == CloudServiceStatus::TIMEOUT)
    {
        Log.info("location cb publish %lu timeout", *(uint32_t *) context);
    }
    else
    {
        Log.info("location cb publish %lu unexpected status: %d", *(uint32_t *) context, status);
    }

    if(issue_callbacks)
    {
        issue_location_publish_callbacks(status, rsp_root, req_event);
    }

    return 0;
}

void TrackerLocation::location_publish()
{
    int rval;
    CloudService &cloud_service = CloudService::instance();

    // maintain cloud service lock across the send to allow us to save off
    // the finalized loc publish to retry on failure
    cloud_service.lock();

    CloudServicePublishFlags cloud_flags =
        (_config_state.process_ack) ? CloudServicePublishFlags::FULL_ACK : CloudServicePublishFlags::NONE;

    if(location_publish_retry_str)
    {
        // publish a retry loc
        rval = cloud_service.send(location_publish_retry_str,
            WITH_ACK,
            cloud_flags,
            &TrackerLocation::location_publish_cb, this,
            CLOUD_DEFAULT_TIMEOUT_MS, &_last_location_publish_sec);
    }
    else
    {
        // publish a new loc (contained in cloud_service buffer)
        rval = cloud_service.send(WITH_ACK,
            cloud_flags,
            &TrackerLocation::location_publish_cb, this,
            CLOUD_DEFAULT_TIMEOUT_MS, &_last_location_publish_sec);
    }

    if(rval == -EBUSY)
    {
        // this implies a transient failure that should recover very
        // quickly (normally another publish in progress blocking lower
        // in the system)
        // save off the generated publish to retry as it has already
        // consumed pending events if applicable
        if(!location_publish_retry_str)
        {
            size_t len = strlen(cloud_service.writer().buffer()) + 1;
            location_publish_retry_str = (char *) malloc(len);
            if(location_publish_retry_str)
            {
                memcpy(location_publish_retry_str, cloud_service.writer().buffer(), len);
            }
            else
            {
                // generated successfuly but unable to save off a copy to retry
                issue_location_publish_callbacks(CloudServiceStatus::FAILURE, NULL, cloud_service.writer().buffer());
            }
        }
    }
    else
    {
        if(rval)
        {
            issue_location_publish_callbacks(CloudServiceStatus::FAILURE, NULL, location_publish_retry_str);
        }
        if(location_publish_retry_str)
        {
            // on success or fatal failure free it
            free(location_publish_retry_str);
            location_publish_retry_str = nullptr;
        }
    }
    cloud_service.unlock();
}

void TrackerLocation::enableNetwork() {
    _sleep.forceFullWakeCycle();
    _gnssStartedSec = System.uptime();
}

void TrackerLocation::enableGnss() {
    LocationService::instance().start();
}

void TrackerLocation::disableGnss() {
    LocationService::instance().stop();
}

void TrackerLocation::enableWifi() {
    WiFi.on();
}

void TrackerLocation::disableWifi() {
    WiFi.off();
}

bool TrackerLocation::isSleepEnabled() {
    return !_sleep.isSleepDisabled();
}

EvaluationResults TrackerLocation::evaluatePublish() {
    auto now = System.uptime();

    if (_pending_immediate) {
        // request for immediate publish overrides the default min/max interval checking
        Log.trace("%s pending_immediate", __FUNCTION__);
        return EvaluationResults {PublishReason::IMMEDIATE, true, false};
    }

    // This will allow a trigger publish on boot.
    // This may be pre-emptively published due to connect and execute times if sleep is enabled.
    // If sleep is disabled then timeout after some time.
    if (_first_publish && !_pending_first_publish) {
        Log.trace("%s first", __FUNCTION__);
        return EvaluationResults {PublishReason::TRIGGERS, true, (now - _gnssStartedSec) < (uint32_t)_sleep.getConfigConnectingTime()};
    }

    uint32_t interval = now - _last_location_publish_sec;
    uint32_t maxInterval = now - _monotonic_publish_sec;

    bool networkNeeded = false;
    uint32_t max = (uint32_t)_config_state.interval_max_seconds;
    auto maxNetwork = max;
    if  (maxNetwork > (uint32_t)_nextEarlyWake) {
        maxNetwork -= (uint32_t)_nextEarlyWake;
    }

    if (_config_state.interval_max_seconds) {
        if (maxInterval >= maxNetwork) {
            // max interval adjusted for early wake
            Log.trace("%s maxNetwork", __FUNCTION__);
            networkNeeded = true;
        }

        if (maxInterval >= max) {
            // max interval and past the max interval so have to publish
            Log.trace("%s max", __FUNCTION__);
            // timeout may be pre-empted when sleep enabled
            return EvaluationResults {PublishReason::TIME, true, (maxInterval - max) < LockTimeoutSec};
        }
    }

    uint32_t min = (uint32_t)_config_state.interval_min_seconds;
    auto minNetwork = min;
    if  (minNetwork > (uint32_t)_nextEarlyWake) {
        minNetwork -= (uint32_t)_nextEarlyWake;
    }

    if (_pending_triggers.size()) {
        if (!_config_state.interval_min_seconds ||
            (interval >= minNetwork)) {
            // min interval adjusted for early wake
            Log.trace("%s minNetwork", __FUNCTION__);
            networkNeeded = true;
        }

        if (!_config_state.interval_min_seconds ||
            (interval >= min)) {
            // no min interval or past the min interval so can publish
            Log.trace("%s min", __FUNCTION__);
            // timeout may be pre-empted when sleep enabled
            return EvaluationResults {PublishReason::TRIGGERS, true, (interval - min) < LockTimeoutSec};
        }
    }

    return EvaluationResults {PublishReason::NONE, networkNeeded, false};
}

// The purpose of thhe sleep prepare callback is to allow each task to calculate
// the next time it needs to wake and process inputs, publish, and what not.
void TrackerLocation::onSleepPrepare(TrackerSleepContext context) {
    // The first thing to figure out is the needed interval, min or max
    unsigned int wake = _last_location_publish_sec;
    int32_t interval = 0;
    if (_pending_triggers.size()) {
        interval = _config_state.interval_min_seconds;
        wake += interval;
    }
    else {
        interval = _config_state.interval_max_seconds;
        wake += interval;
    }

    // Next calculate the early wake offset so that we can wake in the minimum amount of time before
    // the next publish in order to minimize time spent in fully powered operation
    auto t_conn = (uint32_t)_sleep.getConfigConnectingTime();
    if (_sleep.isFullWakeCycle()) {
        uint32_t newEarlyWakeSec = 0;
        uint32_t lastWakeSec = (uint32_t)(context.lastWakeMs + 500) / 1000; // Round ms to s
        if (lastWakeSec >= MiscSleepWakeSec) {
            lastWakeSec -= MiscSleepWakeSec;
        }
        uint32_t wakeToLockDurationSec = 0;
        int32_t publishVariance = 0;

        if (_firstLockSec == 0) {
            wakeToLockDurationSec = t_conn;
        }
        else {
            wakeToLockDurationSec = _firstLockSec - lastWakeSec;
        }

        publishVariance = (int32_t)_last_location_publish_sec - (int32_t)_monotonic_publish_sec;

        newEarlyWakeSec = wakeToLockDurationSec + publishVariance + 1;
        if (newEarlyWakeSec > t_conn) {
            newEarlyWakeSec = t_conn;
        }
        _nextEarlyWake = _earlyWake = newEarlyWakeSec;
    }
    else {  // Not in full wake (modem on)
        _nextEarlyWake = (_earlyWake == 0) ? t_conn : _earlyWake;
    }

    // If the interval and early adjustments puts the wake time in the past then
    // spoil the next sleep attempt and stay awake.
    if (wake > _nextEarlyWake)
        wake -= _nextEarlyWake;

    TrackerSleepError wakeRet = _sleep.wakeAtSeconds(wake);

    if (wakeRet == TrackerSleepError::TIME_IN_PAST) {
        wake = 0; // Force cancelled sleep
        _sleep.wakeAtSeconds(wake);
    }

    Log.trace("TrackerLocation: last=%lu, interval=%ld, wake=%u", _last_location_publish_sec, interval, wake);
}

// The purpose of this callback is to alert us that sleep has been cancelled by another task or improper wake settings.
void TrackerLocation::onSleepCancel(TrackerSleepContext context) {

}

// This callback will alert us that the system is just about to go to sleep.  This is past of the point
// of no return to cancel the pending sleep cycle.
void TrackerLocation::onSleep(TrackerSleepContext context) {
    disableGnss();
    disableWifi();
}

// This callback will be called immediately after wake from sleep and allows us to figure out if the network interface
// is needed and enable it if so.
void TrackerLocation::onWake(TrackerSleepContext context) {
    // Allow capturing of the first lock instance
    _firstLockSec = 0;

    auto result = evaluatePublish();

    if (result.networkNeeded) {
        enableNetwork();
        if (_config_state_loop_safe.gnss) {
            enableGnss();
        }
        if (_config_state_loop_safe.enhance_loc) {
            if (_config_state_loop_safe.wps) {
                enableWifi();
            }
        }
        Log.trace("%s needs to start the network", __FUNCTION__);
    }
    else {
        // Put in our vote to shutdown early
        _sleep.extendExecutionFromNow(EarlySleepSec, true);
    }

    // Ensure the loop runs immediately
    _loopSampleTick = 0;
}

void TrackerLocation::onSleepState(TrackerSleepContext context) {
    switch (context.reason) {
        case TrackerSleepReason::STATE_TO_CONNECTING: {
            Log.trace("%s starting GNSS", __FUNCTION__);
            LocationService::instance().start();
            break;
        }

        case TrackerSleepReason::STATE_TO_SHUTDOWN: {
            Log.trace("%s stopping GNSS for shutdown", __FUNCTION__);
            disableGnss();
            break;
        }
    }
}

int TrackerLocation::parseServeCell(const char* in, CellularServing& out) {
    CellularServing ret;
    char state[16] = {};
    char rat[16] = {};

    out = {};
    auto nitems = sscanf(in, " +QENG: \"servingcell\",\"%15[^\"]\",\"%15[^\"]\",\"%*15[^\"]\","
            "%u,%u,%lX,"
            "%*15[^,],%*15[^,],%*15[^,],%*15[^,],%*15[^,],%X,%d",
            state, rat,
            &ret.mcc, &ret.mnc, &ret.cellId, &ret.tac, &ret.signalPower);

    if (nitems < 7) {
        return SYSTEM_ERROR_NOT_ENOUGH_DATA;
    }

    if (!strncmp(rat, "CAT-M", 5)) {
        out.rat = RadioAccessTechnology::LTE_CAT_M1;
    }
    else if (!strncmp(rat, "LTE", 3)) {
        out.rat = RadioAccessTechnology::LTE;
    }
    else if (!strncmp(rat, "CAT-NB", 6)) {
        out.rat = RadioAccessTechnology::LTE_NB_IOT;
    }
    else {
        return SYSTEM_ERROR_NOT_SUPPORTED;
    }

    out.mcc = ret.mcc;
    out.mnc = ret.mnc;
    out.cellId = ret.cellId;
    out.tac = ret.tac;
    out.signalPower = ret.signalPower;

    return SYSTEM_ERROR_NONE;
}

int TrackerLocation::serving_cb(int type, const char* buf, int len, TrackerLocation* context) {
    if (type == TYPE_OK) {
        return RESP_OK;
    }

    (void)parseServeCell(buf, context->servingTower);
    return WAIT;
}

int TrackerLocation::parseCell(const char* in, CellularNeighbors& out) {
    CellularNeighbors ret;
    char rat[16] = {0};

    auto nitems = sscanf(in, " +QENG: \"neighbourcell %*15[^\"]\",\"%15[^\"]\",%lu,%lu,%d,%d,%d",
            rat,
            &ret.earfcn, &ret.neighborId, &ret.signalQuality, &ret.signalPower, &ret.signalStrength);

    if (nitems < 6) {
        return SYSTEM_ERROR_NOT_ENOUGH_DATA;
    }

    if (!strncmp(rat, "CAT-M", 5)) {
        out.rat = RadioAccessTechnology::LTE_CAT_M1;
    }
    else if (!strncmp(rat, "LTE", 3)) {
        out.rat = RadioAccessTechnology::LTE;
    }
    else if (!strncmp(rat, "CAT-NB", 6)) {
        out.rat = RadioAccessTechnology::LTE_NB_IOT;
    }
    else {
        return SYSTEM_ERROR_NOT_SUPPORTED;
    }

    out.earfcn = ret.earfcn;
    out.neighborId = ret.neighborId;
    out.signalQuality = ret.signalQuality;
    out.signalPower = ret.signalPower;
    out.signalStrength = ret.signalStrength;

    return SYSTEM_ERROR_NONE;
}

int TrackerLocation::neighbor_cb(int type, const char* buf, int len, TrackerLocation* context) {
    if (type == TYPE_OK) {
        return RESP_OK;
    }

    CellularNeighbors neighbor;
    if (parseCell(buf, neighbor) == SYSTEM_ERROR_NONE) {
        context->towerList.append(neighbor);
    }

    return WAIT;
}

size_t TrackerLocation::buildTowerInfo(JSONBufferWriter& writer, size_t size) {
    if (!_config_state_loop_safe.tower) {
        return 0;
    }

    size_t written = writer.dataSize();

    // The cellular information here is always sent and not configurable
    Cellular.command(serving_cb, this, 10000, "AT+QENG=\"servingcell\"\r\n");
    if (servingTower.rat != RadioAccessTechnology::NONE) {
        writer.name("towers").beginArray();
        writer.beginObject();
        writer.name("rat").value("lte");
        writer.name("mcc").value((unsigned)servingTower.mcc);
        writer.name("mnc").value((unsigned)servingTower.mnc);
        writer.name("lac").value((unsigned)servingTower.tac);
        writer.name("cid").value((unsigned)servingTower.cellId);
        writer.name("str").value(servingTower.signalPower);
        writer.endObject();

        towerList.clear();
        (void)Cellular.command(neighbor_cb, this, 10000, "AT+QENG=\"neighbourcell\"\r\n");
        auto towerCount = TrackerLocationMaxTowerSend - 1;  // one has already been taken as the serving tower
        for (auto tower: towerList) {
            if (towerCount-- <= 0) {
                break;
            }
            writer.beginObject();
            writer.name("nid").value((unsigned)tower.neighborId);
            writer.name("ch").value((unsigned)tower.earfcn);
            writer.name("str").value(tower.signalPower);
            writer.endObject();
        }

        writer.endArray();
    }

    return writer.dataSize() - written;
}

void TrackerLocation::wifi_cb(WiFiAccessPoint* wap, TrackerLocation* context) {
    if (context->wpsList.size() < TrackerLocationMaxWpsCollect)
        context->wpsList.append(*wap);
}

size_t TrackerLocation::buildWpsInfo(JSONBufferWriter& writer, size_t size) {
    if (!_config_state_loop_safe.wps) {
        return 0;
    }

    size_t written = writer.dataSize();

    do {
        // This is a work-around to determine how much data is left in the publish message to
        // allow the WPS object to fill up as much information as possible before closing the
        // JSON message.
        if (ObjectEstimateWpsHeaderSize >= size) {
            // There is no use on continuing
            break;
        }

        size_t wpsCount = (size - ObjectEstimateWpsHeaderSize) / ObjectEstimateWpsDataSize;
        if (wpsCount == 0) {
            // There is no use on continuing
            break;
        }

        wpsList.clear();
        (void)WiFi.scan(wifi_cb, this);
        // NOTE: Any sorting of WiFi access points should be performed here
        if (!wpsList.isEmpty()) {
            writer.name("wps").beginArray();
            int wifiCount = wpsCount;
            for (auto ap: wpsList) {
                if (wifiCount-- <= 0) {
                    break;
                }
                String bssid = String::format("%02x:%02x:%02x:%02x:%02x:%02x",
                    ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
                writer.beginObject();
                writer.name("bssid").value(bssid);
                writer.name("ch").value(ap.channel);
                writer.name("str").value(ap.rssi);
                writer.endObject();
            }
            writer.endArray();
        }
    } while (false);

    return writer.dataSize() - written;
}

GnssState TrackerLocation::loopLocation(LocationPoint& cur_loc) {
    if (!_config_state.gnss) {
        return GnssState::DISABLED;
    }
    GnssState currentGnssState = GnssState::ON_LOCKED_STABLE;

    LocationStatus locStatus;
    LocationService::instance().getStatus(locStatus);

    do {
        if (!locStatus.powered) {
            currentGnssState = GnssState::OFF;
            break;
        }

        if (LocationService::instance().getLocation(cur_loc) != SYSTEM_ERROR_NONE)
        {
            currentGnssState = GnssState::ERROR;
            break;
        }

        if (!cur_loc.locked) {
            currentGnssState = GnssState::ON_UNLOCKED;
            break;
        }

        if (!cur_loc.stable) {
            currentGnssState = GnssState::ON_LOCKED_UNSTABLE;
            break;
        }

        float radius;
        LocationService::instance().getRadiusThreshold(radius);
        if (radius) {
            bool outside;
            LocationService::instance().isOutsideRadius(outside, cur_loc);
            if (outside) {
                triggerLocPub(Trigger::NORMAL,"radius");
            }
        }
    } while (false);

    // Detect GNSS locked changes
    if ((currentGnssState == GnssState::ON_LOCKED_STABLE) &&
        (currentGnssState != _lastGnssState)) {

        // Capture the time that the first lock out of sleep happened
        if (_firstLockSec == 0) {
            _firstLockSec = System.uptime();
        }

        // Only publish with "lock" trigger when not sleeping and when enabled to do so
        if (_sleep.isSleepDisabled() && _config_state.lock_trigger) {
            triggerLocPub(Trigger::NORMAL,"lock");
        }
    }

    _lastGnssState = currentGnssState;

    return currentGnssState;
}

void TrackerLocation::buildPublish(LocationPoint& cur_loc) {
    bool locked = (_config_state.gnss) ? cur_loc.locked : false;

    if(locked) {
        LocationService::instance().setWayPoint(cur_loc.latitude, cur_loc.longitude);
    }

    CloudService &cloud_service = CloudService::instance();
    cloud_service.beginCommand("loc");
    cloud_service.writer().name("loc").beginObject();
    if (locked) {
        cloud_service.writer().name("lck").value(1);
        cloud_service.writer().name("time").value((unsigned int) cur_loc.epochTime);
        cloud_service.writer().name("lat").value(cur_loc.latitude, 8);
        cloud_service.writer().name("lon").value(cur_loc.longitude, 8);
        if(!_config_state.min_publish)
        {
            cloud_service.writer().name("alt").value(cur_loc.altitude, 3);
            cloud_service.writer().name("hd").value(cur_loc.heading, 2);
            cloud_service.writer().name("spd").value(cur_loc.speed, 2);
            cloud_service.writer().name("h_acc").value(cur_loc.horizontalAccuracy, 3);
            cloud_service.writer().name("v_acc").value(cur_loc.verticalAccuracy, 3);
        }
    }
    else {
        cloud_service.writer().name("lck").value(0);
    }

    for(auto cb : locGenCallbacks) {
        cb(cloud_service.writer(), cur_loc);
    }

    cloud_service.writer().endObject();

    if(!_pending_triggers.isEmpty()) {
        std::lock_guard<RecursiveMutex> lg(mutex);
        cloud_service.writer().name("trig").beginArray();
        for (auto trigger : _pending_triggers) {
            cloud_service.writer().value(trigger);
        }
        _pending_triggers.clear();
        cloud_service.writer().endArray();
    }

    if (_config_state_loop_safe.enhance_loc) {
        // Request a callback of the enhanced location when made available
        if (_config_state_loop_safe.loc_cb) {
            cloud_service.writer().name("loc_cb").value(true);
        }

        size_t remainingSize = cloud_service.writer().bufferSize() - 1 /* null */
            - cloud_service.writer().dataSize() - cloud_service.estimatedEndCommandSize();

        // Populate cellular tower information for publish
        remainingSize -= buildTowerInfo(cloud_service.writer(), remainingSize);
        remainingSize -= buildWpsInfo(cloud_service.writer(), remainingSize);
    }

    Log.info("%.*s", cloud_service.writer().dataSize(), cloud_service.writer().buffer());
}

void TrackerLocation::loop() {
    // The rest of this loop should only sample as fast as necessary
    if (millis() - _loopSampleTick < LoopSampleRate)
    {
        return;
    }

    bool firstLoop = (_loopSampleTick == 0);
    _loopSampleTick = millis();

    // Sync power state changes
    tracker_location_config_t captureConfig = _config_state;

    if (firstLoop) {
        if (captureConfig.gnss) {
            enableGnss();
        }
        if (captureConfig.enhance_loc && captureConfig.wps) {
            enableWifi();
        }
        else if (captureConfig.enhance_loc && !captureConfig.wps) {
            disableWifi();
        }
    }
    else {
        LocationStatus gnssPoweredStatus;
        LocationService::instance().getStatus(gnssPoweredStatus);
        if (captureConfig.gnss && !gnssPoweredStatus.powered) {
            enableGnss();
        }
        else if (!captureConfig.gnss && gnssPoweredStatus.powered) {
            disableGnss();
        }

        if (captureConfig.enhance_loc && captureConfig.wps && !_config_state_loop_safe.wps) {
            enableWifi();
        }
        else if (captureConfig.enhance_loc && !captureConfig.wps && _config_state_loop_safe.wps) {
            disableWifi();
        }
    }

    // The rest of this loop will depend on a constant setting for GNSS and WiFi condif state
    _config_state_loop_safe = captureConfig;

    // First take care of any retry attempts of last loc
    if (location_publish_retry_str && Particle.connected())
    {
        Log.info("retry failed publish");
        location_publish();
    }

    // Gather current location information and status
    LocationPoint cur_loc;
    auto locationStatus = loopLocation(cur_loc);

    // Perform interval evaluation
    auto publishReason = evaluatePublish();

    // This evaluation may have performed earlier and determined that no network was needed.  Check again
    // because this loop may overlap with required network operations.
    if (!_sleep.isFullWakeCycle() && publishReason.networkNeeded) {
        enableNetwork();
    }

    bool publishNow = false;

    //                                   : NONE      TIME        TRIG        IMM
    //                                    ----------------------------------------
    // GnssState::DISABLED                  NA       PUB         PUB         PUB
    // GnssState::OFF                       NA       PUB         PUB         PUB
    // GnssState::ON_UNLOCKED               NA       WAIT        WAIT        PUB
    // GnssState::ON_LOCKED_UNSTABLE        NA       WAIT        WAIT        PUB
    // GnssState::ON_LOCKED_STABLE          NA       PUB         PUB         PUB

    switch (publishReason.reason) {
        case PublishReason::NONE: {
            // If there is nothing to do then get out
            return;
        }

        case PublishReason::TIME: {
            switch (locationStatus) {
                case GnssState::DISABLED:
                // fall through
                case GnssState::ON_LOCKED_STABLE: {
                    Log.trace("publishing from max interval");
                    triggerLocPub(Trigger::NORMAL,"time");
                    publishNow = true;
                    break;
                }

                case GnssState::OFF:
                // fall through
                case GnssState::ON_UNLOCKED:
                // fall through
                case GnssState::ON_LOCKED_UNSTABLE: {
                    if (!publishReason.lockWait) {
                        Log.trace("publishing from max interval after waiting");
                        triggerLocPub(Trigger::NORMAL,"time");
                        publishNow = true;
                        break;
                    }
                    Log.trace("waiting for stable GNSS lock for max interval");
                    break;
                }
            }
            break;
        }

        case PublishReason::TRIGGERS: {
            switch (locationStatus) {
                case GnssState::DISABLED:
                // fall through
                case GnssState::ON_LOCKED_STABLE: {
                    Log.trace("publishing from triggers");
                    publishNow = true;
                    _newMonotonic = true;
                    break;
                }

                case GnssState::OFF:
                // fall through
                case GnssState::ON_UNLOCKED:
                // fall through
                case GnssState::ON_LOCKED_UNSTABLE: {
                    if (!publishReason.lockWait) {
                        Log.trace("publishing from triggers after waiting");
                        publishNow = true;
                        _newMonotonic = true;
                        break;
                    }
                    Log.trace("waiting for stable GNSS lock for triggers");
                    break;
                }
            }
            break;
        }

        case PublishReason::IMMEDIATE: {
            Log.trace("publishing from immediate");
            _pending_immediate = false;
            publishNow = true;
            _newMonotonic = true;
            break;
        }
    }

    //
    // Perform publish of location data if requested
    //

    // then of any new publish
    if(publishNow && Particle.connected())
    {
        if(location_publish_retry_str)
        {
            Log.info("freeing unsuccessful retry");
            // retried attempt not completed in time for new publish
            // drop and issue callbacks
            issue_location_publish_callbacks(CloudServiceStatus::TIMEOUT, NULL, location_publish_retry_str);
            free(location_publish_retry_str);
            location_publish_retry_str = nullptr;
        }
        Log.info("publishing now...");
        buildPublish(cur_loc);
        pendingLocPubCallbacks = locPubCallbacks;
        locPubCallbacks.clear();
        _last_location_publish_sec = System.uptime();
        if ((_first_publish && !_pending_first_publish) || _newMonotonic)
        {
            _monotonic_publish_sec = _last_location_publish_sec;
            _newMonotonic = false;
        }
        else
        {
            _monotonic_publish_sec += (uint32_t)_config_state.interval_max_seconds;
        }

        // Prevent flooding of first publishes when there are no acknowledges.
        if (!_config_state.process_ack && _first_publish) {
            _first_publish = false;
        }

        location_publish();

        // There may be a delay between the first event being published and an acknowledgement
        // from the cloud.  This leads to multiple event publishes meant to be the first publish.
        if (_first_publish && !_pending_first_publish) {
            _pending_first_publish = true;
        }
    }
}

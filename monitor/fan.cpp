/**
 * Copyright © 2017 IBM Corporation
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
#include "fan.hpp"

#include "logging.hpp"
#include "sdbusplus.hpp"
#include "system.hpp"
#include "types.hpp"
#include "utility.hpp"

#include <fmt/format.h>

#include <phosphor-logging/log.hpp>

#include <algorithm>

namespace phosphor
{
namespace fan
{
namespace monitor
{

using namespace phosphor::logging;
using namespace sdbusplus::bus::match;

Fan::Fan(Mode mode, sdbusplus::bus::bus& bus, const sdeventplus::Event& event,
         std::unique_ptr<trust::Manager>& trust, const FanDefinition& def,
         System& system) :
    _bus(bus),
    _name(std::get<fanNameField>(def)),
    _deviation(std::get<fanDeviationField>(def)),
    _numSensorFailsForNonFunc(std::get<numSensorFailsForNonfuncField>(def)),
    _trustManager(trust),
#ifdef MONITOR_USE_JSON
    _monitorDelay(std::get<monitorStartDelayField>(def)),
    _monitorTimer(event, std::bind(std::mem_fn(&Fan::startMonitor), this)),
#endif
    _system(system),
    _presenceMatch(bus,
                   rules::propertiesChanged(util::INVENTORY_PATH + _name,
                                            util::INV_ITEM_IFACE),
                   std::bind(std::mem_fn(&Fan::presenceChanged), this,
                             std::placeholders::_1)),
    _presenceIfaceAddedMatch(
        bus,
        rules::interfacesAdded() +
            rules::argNpath(0, util::INVENTORY_PATH + _name),
        std::bind(std::mem_fn(&Fan::presenceIfaceAdded), this,
                  std::placeholders::_1)),
    _fanMissingErrorDelay(std::get<fanMissingErrDelayField>(def)),
    _countInterval(std::get<countIntervalField>(def))
{
    bool enableCountTimer = false;

    // Start from a known state of functional (even if
    // _numSensorFailsForNonFunc is 0)
    updateInventory(true);

    // Setup tach sensors for monitoring
    auto& sensors = std::get<sensorListField>(def);
    for (auto& s : sensors)
    {
        _sensors.emplace_back(std::make_shared<TachSensor>(
            mode, bus, *this, std::get<sensorNameField>(s),
            std::get<hasTargetField>(s), std::get<funcDelay>(def),
            std::get<targetInterfaceField>(s), std::get<factorField>(s),
            std::get<offsetField>(s), std::get<methodField>(def),
            std::get<thresholdField>(s), std::get<timeoutField>(def),
            std::get<nonfuncRotorErrDelayField>(def), event));

        _trustManager->registerSensor(_sensors.back());
        if (_sensors.back()->getMethod() == MethodMode::count)
        {
            enableCountTimer = true;
        }
    }

    // If the error checking method will be 'count', then it needs a timer.
    // The timer is repeating but is disabled immediately because it doesn't
    // need to start yet.
    if (enableCountTimer)
    {
        _countTimer = std::make_unique<
            sdeventplus::utility::Timer<sdeventplus::ClockId::Monotonic>>(
            event, std::bind(&Fan::countTimerExpired, this),
            std::chrono::seconds(_countInterval));

        _countTimer->setEnabled(false);
    }

#ifndef MONITOR_USE_JSON
    // Check current tach state when entering monitor mode
    if (mode != Mode::init)
    {
        _monitorReady = true;

        // The TachSensors will now have already read the input
        // and target values, so check them.
        tachChanged();
    }
#else
    if (_system.isPowerOn())
    {
        _monitorTimer.restartOnce(std::chrono::seconds(_monitorDelay));
    }
#endif

    if (_fanMissingErrorDelay)
    {
        _fanMissingErrorTimer = std::make_unique<
            sdeventplus::utility::Timer<sdeventplus::ClockId::Monotonic>>(
            event, std::bind(&System::fanMissingErrorTimerExpired, &system,
                             std::ref(*this)));
    }

    try
    {
        _present = util::SDBusPlus::getProperty<bool>(
            util::INVENTORY_PATH + _name, util::INV_ITEM_IFACE, "Present");

        if (!_present)
        {
            getLogger().log(
                fmt::format("On startup, fan {} is missing", _name));
            if (_system.isPowerOn() && _fanMissingErrorTimer)
            {
                _fanMissingErrorTimer->restartOnce(
                    std::chrono::seconds{*_fanMissingErrorDelay});
            }
        }
    }
    catch (const util::DBusServiceError& e)
    {
        // This could happen on the first BMC boot if the presence
        // detect app hasn't started yet and there isn't an inventory
        // cache yet.
    }
}

void Fan::presenceIfaceAdded(sdbusplus::message::message& msg)
{
    sdbusplus::message::object_path path;
    std::map<std::string, std::map<std::string, std::variant<bool>>> interfaces;

    msg.read(path, interfaces);

    auto properties = interfaces.find(util::INV_ITEM_IFACE);
    if (properties == interfaces.end())
    {
        return;
    }

    auto property = properties->second.find("Present");
    if (property == properties->second.end())
    {
        return;
    }

    _present = std::get<bool>(property->second);

    if (!_present)
    {
        getLogger().log(fmt::format(
            "New fan {} interface added and fan is not present", _name));
        if (_system.isPowerOn() && _fanMissingErrorTimer)
        {
            _fanMissingErrorTimer->restartOnce(
                std::chrono::seconds{*_fanMissingErrorDelay});
        }
    }

    _system.fanStatusChange(*this);
}

void Fan::startMonitor()
{
    _monitorReady = true;

    if (_countTimer)
    {
        _countTimer->resetRemaining();
        _countTimer->setEnabled(true);
    }

    std::for_each(_sensors.begin(), _sensors.end(), [this](auto& sensor) {
        if (_present)
        {
            try
            {
                // Force a getProperty call to check if the tach sensor is
                // on D-Bus.  If it isn't, now set it to nonfunctional.
                // This isn't done earlier so that code watching for
                // nonfunctional tach sensors doesn't take actions before
                // those sensors show up on D-Bus.
                sensor->updateTachAndTarget();
                tachChanged(*sensor);
            }
            catch (const util::DBusServiceError& e)
            {
                // The tach property still isn't on D-Bus, ensure
                // sensor is nonfunctional.
                getLogger().log(fmt::format(
                    "Monitoring starting but {} sensor value not on D-Bus",
                    sensor->name()));

                sensor->setFunctional(false);

                if (_numSensorFailsForNonFunc)
                {
                    if (_functional && (countNonFunctionalSensors() >=
                                        _numSensorFailsForNonFunc))
                    {
                        updateInventory(false);
                    }
                }

                _system.fanStatusChange(*this);
            }
        }
    });
}

void Fan::tachChanged()
{
    if (_monitorReady)
    {
        for (auto& s : _sensors)
        {
            tachChanged(*s);
        }
    }
}

void Fan::tachChanged(TachSensor& sensor)
{
    if (!_system.isPowerOn() || !_monitorReady)
    {
        return;
    }

    if (_trustManager->active())
    {
        if (!_trustManager->checkTrust(sensor))
        {
            return;
        }
    }

    // If using the timebased method to determine functional status,
    // check now, otherwise let _countTimer handle it.  A timer is
    // used for the count method so that stuck sensors will continue
    // to be checked.
    if (sensor.getMethod() == MethodMode::timebased)
    {
        process(sensor);
    }
}

void Fan::countTimerExpired()
{
    // For sensors that use the 'count' method, time to check their
    // status and increment/decrement counts as necessary.
    for (auto& sensor : _sensors)
    {
        if (_trustManager->active() && !_trustManager->checkTrust(*sensor))
        {
            continue;
        }
        process(*sensor);
    }
}

void Fan::process(TachSensor& sensor)
{
    // If this sensor is out of range at this moment, start
    // its timer, at the end of which the inventory
    // for the fan may get updated to not functional.

    // If this sensor is OK, put everything back into a good state.

    if (outOfRange(sensor))
    {
        if (sensor.functional())
        {
            switch (sensor.getMethod())
            {
                case MethodMode::timebased:
                    // Start nonfunctional timer if not already running
                    sensor.startTimer(TimerMode::nonfunc);
                    break;
                case MethodMode::count:
                    sensor.setCounter(true);
                    if (sensor.getCounter() >= sensor.getThreshold())
                    {
                        updateState(sensor);
                    }
                    break;
            }
        }
    }
    else
    {
        switch (sensor.getMethod())
        {
            case MethodMode::timebased:
                if (sensor.functional())
                {
                    if (sensor.timerRunning())
                    {
                        sensor.stopTimer();
                    }
                }
                else
                {
                    // Start functional timer if not already running
                    sensor.startTimer(TimerMode::func);
                }
                break;
            case MethodMode::count:
                sensor.setCounter(false);
                if (!sensor.functional() && sensor.getCounter() == 0)
                {
                    updateState(sensor);
                }
                break;
        }
    }
}

uint64_t Fan::findTargetSpeed()
{
    uint64_t target = 0;
    // The sensor doesn't support a target,
    // so get it from another sensor.
    auto s = std::find_if(_sensors.begin(), _sensors.end(),
                          [](const auto& s) { return s->hasTarget(); });

    if (s != _sensors.end())
    {
        target = (*s)->getTarget();
    }

    return target;
}

size_t Fan::countNonFunctionalSensors()
{
    return std::count_if(_sensors.begin(), _sensors.end(),
                         [](const auto& s) { return !s->functional(); });
}

bool Fan::outOfRange(const TachSensor& sensor)
{
    auto actual = static_cast<uint64_t>(sensor.getInput());
    auto range = sensor.getRange(_deviation);

    if ((actual < range.first) || (actual > range.second))
    {
        return true;
    }

    return false;
}

void Fan::updateState(TachSensor& sensor)
{
    auto range = sensor.getRange(_deviation);

    if (!_system.isPowerOn())
    {
        return;
    }

    sensor.setFunctional(!sensor.functional());
    getLogger().log(
        fmt::format("Setting tach sensor {} functional state to {}. "
                    "[target = {}, input = {}, allowed range = ({} - {})]",
                    sensor.name(), sensor.functional(), sensor.getTarget(),
                    sensor.getInput(), range.first, range.second));

    // A zero value for _numSensorFailsForNonFunc means we aren't dealing
    // with fan FRU functional status, only sensor functional status.
    if (_numSensorFailsForNonFunc)
    {
        auto numNonFuncSensors = countNonFunctionalSensors();
        // If the fan was nonfunctional and enough sensors are now OK,
        // the fan can be set to functional
        if (!_functional && !(numNonFuncSensors >= _numSensorFailsForNonFunc))
        {
            getLogger().log(fmt::format("Setting fan {} to functional, number "
                                        "of nonfunctional sensors = {}",
                                        _name, numNonFuncSensors));
            updateInventory(true);
        }

        // If the fan is currently functional, but too many
        // contained sensors are now nonfunctional, update
        // the fan to nonfunctional.
        if (_functional && (numNonFuncSensors >= _numSensorFailsForNonFunc))
        {
            getLogger().log(fmt::format("Setting fan {} to nonfunctional, "
                                        "number of nonfunctional sensors = {}",
                                        _name, numNonFuncSensors));
            updateInventory(false);
        }
    }

    _system.fanStatusChange(*this);
}

void Fan::updateInventory(bool functional)
{
    auto objectMap =
        util::getObjMap<bool>(_name, util::OPERATIONAL_STATUS_INTF,
                              util::FUNCTIONAL_PROPERTY, functional);
    auto response = util::SDBusPlus::lookupAndCallMethod(
        _bus, util::INVENTORY_PATH, util::INVENTORY_INTF, "Notify", objectMap);
    if (response.is_method_error())
    {
        log<level::ERR>("Error in Notify call to update inventory");
        return;
    }

    // This will always track the current state of the inventory.
    _functional = functional;
}

void Fan::presenceChanged(sdbusplus::message::message& msg)
{
    std::string interface;
    std::map<std::string, std::variant<bool>> properties;

    msg.read(interface, properties);

    auto presentProp = properties.find("Present");
    if (presentProp != properties.end())
    {
        _present = std::get<bool>(presentProp->second);

        getLogger().log(
            fmt::format("Fan {} presence state change to {}", _name, _present));

        _system.fanStatusChange(*this);

        if (_fanMissingErrorDelay)
        {
            if (!_present && _system.isPowerOn())
            {
                _fanMissingErrorTimer->restartOnce(
                    std::chrono::seconds{*_fanMissingErrorDelay});
            }
            else if (_present && _fanMissingErrorTimer->isEnabled())
            {
                _fanMissingErrorTimer->setEnabled(false);
            }
        }
    }
}

void Fan::sensorErrorTimerExpired(const TachSensor& sensor)
{
    if (_present && _system.isPowerOn())
    {
        _system.sensorErrorTimerExpired(*this, sensor);
    }
}

void Fan::powerStateChanged(bool powerStateOn)
{
#ifdef MONITOR_USE_JSON
    if (powerStateOn)
    {
        _monitorTimer.restartOnce(std::chrono::seconds(_monitorDelay));

        if (_present)
        {
            std::for_each(
                _sensors.begin(), _sensors.end(), [this](auto& sensor) {
                    try
                    {
                        // Force a getProperty call.  If sensor is on D-Bus,
                        // then make sure it's functional.
                        sensor->updateTachAndTarget();

                        // If not functional, set it back to functional.
                        if (!sensor->functional())
                        {
                            sensor->setFunctional(true);
                            _system.fanStatusChange(*this, true);
                        }

                        // Set the counters back to zero
                        if (sensor->getMethod() == MethodMode::count)
                        {
                            sensor->resetMethod();
                        }
                    }
                    catch (const util::DBusServiceError& e)
                    {
                        // Properties still aren't on D-Bus.  Let startMonitor()
                        // deal with it.
                        getLogger().log(fmt::format(
                            "At power on, tach sensor {} value not on D-Bus",
                            sensor->name()));
                    }
                });

            // If configured to change functional state on the fan itself,
            // Set it back to true now if necessary.
            if (_numSensorFailsForNonFunc)
            {
                if (!_functional &&
                    (countNonFunctionalSensors() < _numSensorFailsForNonFunc))
                {
                    updateInventory(true);
                }
            }
        }
        else
        {
            getLogger().log(
                fmt::format("At power on, fan {} is missing", _name));

            if (_fanMissingErrorTimer)
            {
                _fanMissingErrorTimer->restartOnce(
                    std::chrono::seconds{*_fanMissingErrorDelay});
            }
        }
    }
    else
    {
        _monitorReady = false;

        if (_monitorTimer.isEnabled())
        {
            _monitorTimer.setEnabled(false);
        }

        if (_fanMissingErrorTimer && _fanMissingErrorTimer->isEnabled())
        {
            _fanMissingErrorTimer->setEnabled(false);
        }

        std::for_each(_sensors.begin(), _sensors.end(), [](auto& sensor) {
            if (sensor->timerRunning())
            {
                sensor->stopTimer();
            }
        });

        if (_countTimer)
        {
            _countTimer->setEnabled(false);
        }
    }
#endif
}

} // namespace monitor
} // namespace fan
} // namespace phosphor

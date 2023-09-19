/**
 * Copyright © 2020 IBM Corporation
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
#pragma once

#include "sdbusplus.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdeventplus/source/signal.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

namespace phosphor::fan
{

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace phosphor::logging;

constexpr auto confOverridePath = "/etc/phosphor-fan-presence";
constexpr auto confBasePath = "/usr/share/phosphor-fan-presence";
constexpr auto confCompatServ = "xyz.openbmc_project.EntityManager";
# ifdef USE_IBM_COMPATIBLE_SYSTEM
constexpr auto confCompatIntf =
    "xyz.openbmc_project.Configuration.IBMCompatibleSystem";
constexpr auto confCompatProp = "Names";
# else
// PDI description: Implement to provide basic item attributes.
// Required by all objects within the inventory namespace.
constexpr auto confCompatIntf = "xyz.openbmc_project.Inventory.Item";
constexpr auto confCompatProp = "PrettyName";
#endif

/**
 * @class NoConfigFound - A no JSON configuration found exception
 *
 * A no JSON configuration found exception that is used to denote that a JSON
 * configuration has not been found yet.
 */
class NoConfigFound : public std::runtime_error
{
  public:
    NoConfigFound() = delete;
    NoConfigFound(const NoConfigFound&) = delete;
    NoConfigFound(NoConfigFound&&) = delete;
    NoConfigFound& operator=(const NoConfigFound&) = delete;
    NoConfigFound& operator=(NoConfigFound&&) = delete;
    ~NoConfigFound() = default;

    /**
     * @brief No JSON configuration found exception object
     *
     * When attempting to find the JSON configuration file(s), a NoConfigFound
     * exception can be thrown to denote that at that time finding/loading the
     * JSON configuration file(s) for a fan application failed. Details on what
     * application and JSON configuration file that failed to be found will be
     * logged resulting in the application being terminated.
     *
     * @param[in] details - Additional details
     */
    NoConfigFound(const std::string& appName, const std::string& fileName) :
        std::runtime_error(std::format("JSON configuration not found [Could "
                                       "not find fan {} conf file {}]",
                                       appName, fileName)
                               .c_str())
    {}
};

class JsonConfig
{
  public:
    /**
     * @brief Get the object paths with the compatible interface
     *
     * Retrieve all the object paths implementing the compatible interface for
     * configuration file loading.
     */
    static auto& getCompatObjPaths() __attribute__((pure))
    {
        static auto paths = util::SDBusPlus::getSubTreePathsRaw(
            util::SDBusPlus::getBus(), "/", confCompatIntf, 0);
        return paths;
    }

    /**
     * @brief Constructor
     *
     * Attempts to set the list of compatible values from the compatible
     * interface and call the fan app's function to load its config file(s). If
     * the compatible interface is not found, it subscribes to the
     * interfacesAdded signal for that interface on the compatible service
     * defined above.
     *
     * @param[in] func - Fan app function to call to load its config file(s)
     */
    JsonConfig(std::function<void()> func) : _loadFunc(func)
    {
        std::cerr << "Chau: JsonConfig()\n";
        std::vector<std::string> compatObjPaths;

        _match = std::make_unique<sdbusplus::bus::match_t>(
            util::SDBusPlus::getBus(),
            sdbusplus::bus::match::rules::interfacesAdded() +
                sdbusplus::bus::match::rules::sender(confCompatServ),
            std::bind(&JsonConfig::compatIntfAdded, this,
                      std::placeholders::_1));

        try
        {
            compatObjPaths = getCompatObjPaths();
        }
        catch (const util::DBusMethodError&)
        {
            // Compatible interface does not exist on any dbus object yet
        }

        if (!compatObjPaths.empty())
        {
            for (auto& path : compatObjPaths)
            {
                try
                {
#ifdef USE_IBM_COMPATIBLE_SYSTEM
                    // Retrieve json config compatible relative path
                    // locations (last one found will be what's used if more
                    // than one dbus object implementing the compatible
                    // interface exists).
                    _confCompatValues =
                        util::SDBusPlus::getProperty<std::vector<std::string>>(
                            util::SDBusPlus::getBus(), path, confCompatIntf,
                            confCompatProp);
#else
                    // There should be more than one service and object path
                    // implementing this interface. Since the target property
                    // is a string, archive all the found results to look for
                    // the correct one later.
                    std::string propValue =
                        util::SDBusPlus::getProperty<std::string>(
                            util::SDBusPlus::getBus(), path, confCompatIntf,
                            confCompatProp);
                    _confCompatValues.emplace_back(propValue);
#endif
                }
                catch (const util::DBusError&)
                {
                    // Compatible property unavailable on this dbus object
                    // path's compatible interface, ignore
                }
            }
        }

        // Check if required config(s) are found, otherwise this is intended to
        // catch the exception thrown by the getConfFile function when the
        // required config file was not found. This would then result in waiting
        // for the compatible interfacesAdded signal
        try
        {
            std::cerr << "Chau: _loadFunc()\n";
            _loadFunc();
        }
        catch (const NoConfigFound&)
        {
            std::cerr << "Chau: NoConfigFound, wait for compatible interfacesAdded signal\n";
            // Wait for compatible interfacesAdded signal
        }
    }

    /**
     * @brief InterfacesAdded callback function for the compatible interface.
     *
     * @param[in] msg - The D-Bus message contents
     *
     * If the compatible interface is found, it uses the compatible property on
     * the interface to set the list of compatible values to be used when
     * attempting to get a configuration file. Once the list of compatible
     * values has been updated, it calls the load function.
     */
    void compatIntfAdded(sdbusplus::message_t& msg)
    {
        std::cerr << "Chau: compatIntfAdded()\n";
#ifndef USE_IBM_COMPATIBLE_SYSTEM
        // Avoid processing any interface added when there's already a
        // valid folder name to look for configs. _validCompatValue is
        // cleared when getConfFile() fails to find configs under this
        // folder name.
        if (!_validCompatValue.empty())
        {
            std::cerr << "Chau: _validCompatValue is not empty " << _validCompatValue << "\n";
            return;
        }
#endif
        std::cerr << "Chau: _validCompatValue is empty\n";
        sdbusplus::message::object_path op;
        std::map<std::string,
                 std::map<std::string, std::variant<std::vector<std::string>, std::string>>>
            intfProps;

        msg.read(op, intfProps);

        if (intfProps.find(confCompatIntf) == intfProps.end())
        {
            std::cerr << "Chau: can't find target inf\n";
            return;
        }

        const auto& props = intfProps.at(confCompatIntf);

#ifdef USE_IBM_COMPATIBLE_SYSTEM
        // Only one dbus object with the compatible interface is used at a time
        _confCompatValues =
            std::get<std::vector<std::string>>(props.at(confCompatProp));
#else
        std::string propValue =
            std::get<std::string>(props.at(confCompatProp));
        _confCompatValues.emplace_back(propValue);
#endif
        _loadFunc();
    }

    /**
     * Get the json configuration file. The first location found to contain
     * the json config file for the given fan application is used from the
     * following locations in order.
     * 1.) From the confOverridePath location
     * 2.) From the default confBasePath location
     * 3.) From config file found using an entry from a list obtained from an
     * interface's property as a relative path extension on the base path where:
     *     interface = Interface set in confCompatIntf with the property
     *     property = Property set in confCompatProp containing a list of
     *                subdirectories in priority order to find a config
     *
     * @brief Get the configuration file to be used
     *
     * @param[in] appName - The phosphor-fan-presence application name
     * @param[in] fileName - Application's configuration file's name
     * @param[in] isOptional - Config file is optional, default to 'false'
     *
     * @return filesystem path
     *     The filesystem path to the configuration file to use
     */
    static const fs::path getConfFile(const std::string& appName,
                                      const std::string& fileName,
                                      bool isOptional = false)
    {
        std::cerr << "Chau: getConfFile() for " << appName << " " << fileName  << "\n";
        // Check override location
        fs::path confFile = fs::path{confOverridePath} / appName / fileName;
        if (fs::exists(confFile))
        {
            std::cerr << "Chau: default\n";
            return confFile;
        }

        // If the default file is there, use it
        confFile = fs::path{confBasePath} / appName / fileName;
        if (fs::exists(confFile))
        {
            std::cerr << "Chau: default\n";
            return confFile;
        }

        // Look for a config file at each entry relative to the base
        // path and use the first one found
        std::cerr << "Chau confFile :\n";
        auto it =
            std::find_if(_confCompatValues.begin(), _confCompatValues.end(),
                         [&confFile, &appName, &fileName](const auto& value) {
            confFile = fs::path{confBasePath} / appName / value / fileName;
            std::cerr << " " << confFile << " ";
            _validCompatValue = value;
            return fs::exists(confFile);
            });

        std::cerr << "\n";

        if (it == _confCompatValues.end())
        {
            std::cerr << "Chau: Can't find config it == _confCompatValues.end()\n";
            confFile.clear();
            _validCompatValue.clear();
        }

        if (confFile.empty() && !isOptional)
        {
            throw NoConfigFound(appName, fileName);
        }

        std::cerr << "Chau: _validCompatValue: " << _validCompatValue << "\n";

        return confFile;
    }

    /**
     * @brief Load the JSON config file
     *
     * @param[in] confFile - File system path of the configuration file to load
     *
     * @return Parsed JSON object
     *     The parsed JSON configuration file object
     */
    static const json load(const fs::path& confFile)
    {
        std::ifstream file;
        json jsonConf;

        if (!confFile.empty() && fs::exists(confFile))
        {
            log<level::INFO>(
                std::format("Loading configuration from {}", confFile.string())
                    .c_str());
            file.open(confFile);
            try
            {
                // Enable ignoring `//` or `/* */` comments
                jsonConf = json::parse(file, nullptr, true, true);
            }
            catch (const std::exception& e)
            {
                log<level::ERR>(
                    std::format(
                        "Failed to parse JSON config file: {}, error: {}",
                        confFile.string(), e.what())
                        .c_str());
                throw std::runtime_error(
                    std::format(
                        "Failed to parse JSON config file: {}, error: {}",
                        confFile.string(), e.what())
                        .c_str());
            }
        }
        else
        {
            log<level::ERR>(std::format("Unable to open JSON config file: {}",
                                        confFile.string())
                                .c_str());
            throw std::runtime_error(
                std::format("Unable to open JSON config file: {}",
                            confFile.string())
                    .c_str());
        }

        return jsonConf;
    }

    /**
     * @brief Return the compatible values property
     *
     * @return const std::vector<std::string>& - The values
     */
    static const std::vector<std::string>& getCompatValues()
    {
        return _confCompatValues;
    }

  private:
    /* Load function to call for a fan app to load its config file(s). */
    std::function<void()> _loadFunc;

    /**
     * @brief The interfacesAdded match that is used to wait
     *        for the IBMCompatibleSystem interface to show up.
     */
    std::unique_ptr<sdbusplus::bus::match_t> _match;

    /**
     * @brief List of compatible values from the compatible interface
     *
     * Only supports a single instance of the compatible interface on a dbus
     * object. If more than one dbus object exists with the compatible
     * interface, the last one found will be the list of compatible values used.
     */
    inline static std::vector<std::string> _confCompatValues;

    inline static std::string _validCompatValue;

};

} // namespace phosphor::fan

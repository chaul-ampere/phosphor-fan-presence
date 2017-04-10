#include <iostream>
#include <memory>
#include <sdbusplus/bus.hpp>
#include <phosphor-logging/log.hpp>
#include "argument.hpp"
#include "cooling_type.hpp"

using namespace phosphor::cooling::type;
using namespace phosphor::logging;

int main(int argc, char* argv[])
{
    auto rc = -1;
    auto options = ArgumentParser(argc, argv);

    auto objpath = (options)["path"];
    if (argc < 2)
    {
        std::cerr << std::endl << "Too few arguments" << std::endl;
        log<level::ERR>("Too few arguments");
        options.usage(argv);
    }
    else if (objpath == ArgumentParser::empty_string)
    {
        log<level::ERR>("Bus path argument required");
    }
    else
    {
        auto bus = sdbusplus::bus::new_default();
        CoolingType coolingType(bus);

        try
        {
            auto air = (options)["air"];
            if (air != ArgumentParser::empty_string)
            {
                coolingType.setAirCooled();
            }

            auto water = (options)["water"];
            if (water != ArgumentParser::empty_string)
            {
                coolingType.setWaterCooled();
            }

            auto gpiopath = (options)["dev"];
            if (gpiopath != ArgumentParser::empty_string)
            {
                auto keycode = (options)["event"];
                if (keycode != ArgumentParser::empty_string)
                {
                    auto gpiocode = std::stoul(keycode);
                    coolingType.readGpio(gpiopath, gpiocode);
                }
                else
                {
                    log<level::ERR>("--event=<keycode> argument required\n");
                    exit(-1);
                }
            }

            coolingType.updateInventory(objpath);
            rc = 0;
        }

        catch (std::exception& err)
        {
            rc = -1;
            log<phosphor::logging::level::ERR>(err.what());
        }

    }

    return rc;
}

// vim: tabstop=8 expandtab shiftwidth=4 softtabstop=4

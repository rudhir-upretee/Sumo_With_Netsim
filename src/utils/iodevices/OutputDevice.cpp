/****************************************************************************/
/// @file    OutputDevice.cpp
/// @author  Daniel Krajzewicz
/// @author  Jakob Erdmann
/// @author  Michael Behrisch
/// @date    2004
/// @version $Id: OutputDevice.cpp 13107 2012-12-02 13:57:34Z behrisch $
///
// Static storage of an output device and its base (abstract) implementation
/****************************************************************************/
// SUMO, Simulation of Urban MObility; see http://sumo.sourceforge.net/
// Copyright (C) 2001-2012 DLR (http://www.dlr.de/) and contributors
/****************************************************************************/
//
//   This file is part of SUMO.
//   SUMO is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
/****************************************************************************/


// ===========================================================================
// included modules
// ===========================================================================
#ifdef _MSC_VER
#include <windows_config.h>
#else
#include <config.h>
#endif

#include <map>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include "OutputDevice.h"
#include "OutputDevice_File.h"
#include "OutputDevice_COUT.h"
#include "OutputDevice_CERR.h"
#include "OutputDevice_Network.h"
#include "PlainXMLFormatter.h"
#include <utils/common/TplConvert.h>
#include <utils/common/UtilExceptions.h>
#include <utils/common/FileHelpers.h>
#include <utils/common/ToString.h>
#include <utils/options/OptionsCont.h>

#ifdef CHECK_MEMORY_LEAKS
#include <foreign/nvwa/debug_new.h>
#endif // CHECK_MEMORY_LEAKS


// ===========================================================================
// static member definitions
// ===========================================================================
std::map<std::string, OutputDevice*> OutputDevice::myOutputDevices;


// ===========================================================================
// static method definitions
// ===========================================================================
OutputDevice&
OutputDevice::getDevice(const std::string& name,
                        const std::string& base) {
    std::string internalName = name;
    if (name == "-") {
        internalName = "stdout";
    }
    // check whether the device has already been aqcuired
    if (myOutputDevices.find(internalName) != myOutputDevices.end()) {
        return *myOutputDevices[internalName];
    }
    // build the device
    OutputDevice* dev = 0;
    // check whether the device shall print to stdout
    if (internalName == "stdout") {
        dev = OutputDevice_COUT::getDevice();
    } else if (internalName == "stderr") {
        dev = OutputDevice_CERR::getDevice();
    } else if (FileHelpers::isSocket(internalName)) {
        try {
            int port = TplConvert::_2int(internalName.substr(internalName.find(":") + 1).c_str());
            dev = new OutputDevice_Network(internalName.substr(0, internalName.find(":")), port);
        } catch (NumberFormatException&) {
            throw IOError("Given port number '" + internalName.substr(internalName.find(":") + 1) + "' is not numeric.");
        } catch (EmptyData&) {
            throw IOError("No port number given.");
        }
    } else {
        const size_t len = internalName.length();
        dev = new OutputDevice_File(FileHelpers::checkForRelativity(internalName, base),
                                    len > 4 && internalName.substr(len - 4) == ".sbx");
    }
    dev->setPrecision();
    dev->getOStream() << std::setiosflags(std::ios::fixed);
    myOutputDevices[internalName] = dev;
    return *dev;
}


bool
OutputDevice::createDeviceByOption(const std::string& optionName,
                                   const std::string& rootElement) {
    if (!OptionsCont::getOptions().isSet(optionName)) {
        return false;
    }
    OutputDevice& dev = OutputDevice::getDevice(OptionsCont::getOptions().getString(optionName));
    if (rootElement != "") {
        dev.writeXMLHeader(rootElement);
    }
    return true;
}


OutputDevice&
OutputDevice::getDeviceByOption(const std::string& optionName) throw(IOError, InvalidArgument) {
    std::string devName = OptionsCont::getOptions().getString(optionName);
    if (myOutputDevices.find(devName) == myOutputDevices.end()) {
        throw InvalidArgument("Device '" + devName + "' has not been created.");
    }
    return OutputDevice::getDevice(devName);
}


void
OutputDevice::closeAll() {
    while (myOutputDevices.size() != 0) {
        myOutputDevices.begin()->second->close();
    }
    myOutputDevices.clear();
}


std::string
OutputDevice::realString(const SUMOReal v, const int precision) {
    std::ostringstream oss;
    if (v == 0) {
        return "0";
    }
    if (v < pow(10., -precision)) {
        oss.setf(std::ios::scientific, std::ios::floatfield);
    } else {
        oss.setf(std::ios::fixed , std::ios::floatfield);    // use decimal format
        oss.setf(std::ios::showpoint);    // print decimal point
        oss << std::setprecision(precision);
    }
    oss << v;
    return oss.str();
}


// ===========================================================================
// member method definitions
// ===========================================================================
OutputDevice::OutputDevice(const bool binary, const unsigned int defaultIndentation)
    : myAmBinary(binary) {
    if (binary) {
        myFormatter = new BinaryFormatter();
    } else {
        myFormatter = new PlainXMLFormatter(defaultIndentation);
    }
}


OutputDevice::~OutputDevice() {
    delete myFormatter;
}


bool
OutputDevice::ok() {
    return getOStream().good();
}


void
OutputDevice::close() {
    while (closeTag()) {}
    for (std::map<std::string, OutputDevice*>::iterator i = myOutputDevices.begin(); i != myOutputDevices.end(); ++i) {
        if (i->second == this) {
            myOutputDevices.erase(i);
            break;
        }
    }
    delete this;
}


void
OutputDevice::setPrecision(unsigned int precision) {
    getOStream() << std::setprecision(precision);
}


bool
OutputDevice::writeXMLHeader(const std::string& rootElement, const std::string xmlParams,
                             const std::string& attrs, const std::string& comment) {
    return myFormatter->writeXMLHeader(getOStream(), rootElement, xmlParams, attrs, comment);
}


OutputDevice&
OutputDevice::openTag(const std::string& xmlElement) {
    myFormatter->openTag(getOStream(), xmlElement);
    return *this;
}


OutputDevice&
OutputDevice::openTag(const SumoXMLTag& xmlElement) {
    myFormatter->openTag(getOStream(), xmlElement);
    return *this;
}


void
OutputDevice::closeOpener() {
    myFormatter->closeOpener(getOStream());
}


bool
OutputDevice::closeTag(bool abbreviated) {
    if (myFormatter->closeTag(getOStream(), abbreviated)) {
        postWriteHook();
        return true;
    }
    return false;
}


void
OutputDevice::postWriteHook() {}


void
OutputDevice::inform(const std::string& msg, const char progress) {
    if (progress != 0) {
        getOStream() << msg << progress;
    } else {
        getOStream() << msg << '\n';
    }
    postWriteHook();
}


OutputDevice&
OutputDevice::writeAttr(std::string attr, std::string val) {
    myFormatter->writeAttr(getOStream(), attr, val);
    return *this;
}

/****************************************************************************/


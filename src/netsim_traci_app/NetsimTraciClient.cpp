/****************************************************************************/
/****************************************************************************/
/* =========================================================================
 * included modules
 * ======================================================================= */
#ifdef _MSC_VER
#include <windows_config.h>
#else
#include "config.h"
#endif

#include <algorithm>
#include <vector>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdlib>

#define BUILD_TCPIP
#include "storage.h"
#include "socket.h"

#include "TraCIConstants.h"
#include "SUMOTime.h"
#include "NetsimTraciClient.h"
#include "MSVehicleStateTable.h"

#ifdef CHECK_MEMORY_LEAKS
#include <foreign/nvwa/debug_new.h>
#endif // CHECK_MEMORY_LEAKS


// ===========================================================================
// used namespaces
// ===========================================================================
using namespace netsimtraciclient;


// ===========================================================================
// method definitions
// ===========================================================================
NetsimTraciClient::NetsimTraciClient(MSVehicleStateTable* ptrVehStateTble,
                                    std::string outputFileName)
    : m_ptrVehStateTbl(ptrVehStateTble),
      outputFileName(outputFileName),
      answerLog("") {

    answerLog.setf(std::ios::fixed , std::ios::floatfield); // use decimal format
    answerLog.setf(std::ios::showpoint); // print decimal point
    answerLog << std::setprecision(2);

    // Only for testing
    m_ptrVehStateTbl->testFillVSTable();
}

NetsimTraciClient::~NetsimTraciClient() {
    writeResult();
}

bool
NetsimTraciClient::run(int port, std::string host, const std::string& strBeginTime, const std::string& strEndTime) {
    std::stringstream msg;
    SUMOTime currTimeInSec = string2time(strBeginTime)/1000;
    SUMOTime endTimeInSec = string2time(strEndTime)/1000;
    std::cout << currTimeInSec << " " << endTimeInSec << std::endl;

    // try to connect
    try {
        TraCIAPI::connect(host, port);
    } catch (tcpip::SocketException& e) {
        std::stringstream msg;
        msg << "#Error while connecting: " << e.what();
        errorMsg(msg);
        return false;
    }

    // Subscribe command ID_LIST to list ids of all vehicles
    // currently running within the scenario. This is one time
    // subscription.
    commandSubscribeIdList(currTimeInSec, endTimeInSec);

    // Main loop for driving SUMO simulation
    for(; currTimeInSec <= endTimeInSec; currTimeInSec++)
        {
        std::cout << "currTimeInSec " << currTimeInSec << std::endl;

        clearActiveLists();

        // Advance simulation
        commandSimulationStep(currTimeInSec);

        displayActiveLists();

        // Subscribe command VAR_SPEED and VAR_POSITION for every
        // new vehicle that entered the simulation
        commandSubscribeSpeedAndPos(currTimeInSec, endTimeInSec);

        // Update Ns3 Mobility Model

        // Send Vehicle state table to SUMO. This table is
        // updated by Ns3 applications running on different nodes.
        commandSetValueVehicleStateTable();
        }

    commandClose();
    close();
    writeResult();
    return true;
}

// ---------- Commands handling
void
NetsimTraciClient::commandSimulationStep(SUMOTime time) {
    send_commandSimulationStep(time);
    answerLog << std::endl << "-> Command sent: <SimulationStep2>:" << std::endl;
    tcpip::Storage inMsg;
    try {
        std::string acknowledgement;
        check_resultState(inMsg, CMD_SIMSTEP2, false, &acknowledgement);
        answerLog << acknowledgement << std::endl;
        validateSimulationStep2(inMsg);
    } catch (tcpip::SocketException& e) {
        answerLog << e.what() << std::endl;
    }
}


void
NetsimTraciClient::commandClose() {
    send_commandClose();
    answerLog << std::endl << "-> Command sent: <Close>:" << std::endl;
    try {
        tcpip::Storage inMsg;
        std::string acknowledgement;
        check_resultState(inMsg, CMD_CLOSE, false, &acknowledgement);
        answerLog << acknowledgement << std::endl;
    } catch (tcpip::SocketException& e) {
        answerLog << e.what() << std::endl;
    }
}


void
NetsimTraciClient::commandGetVariable(int domID, int varID, const std::string& objID, tcpip::Storage* addData) {
    send_commandGetVariable(domID, varID, objID, addData);
    answerLog << std::endl << "-> Command sent: <GetVariable>:" << std::endl
              << "  domID=" << domID << " varID=" << varID
              << " objID=" << objID << std::endl;
    tcpip::Storage inMsg;
    try {
        std::string acknowledgement;
        check_resultState(inMsg, domID, false, &acknowledgement);
        answerLog << acknowledgement << std::endl;
    } catch (tcpip::SocketException& e) {
        answerLog << e.what() << std::endl;
        return;
    }
    check_commandGetResult(inMsg, domID, -1, false);
    // report result state
    try {
        int cmdID = (domID + 0x10);
        int variableID = inMsg.readUnsignedByte();
        std::string objectID = inMsg.readString();
        answerLog <<  "  CommandID=" << cmdID << "  VariableID=" << variableID << "  ObjectID=" << objectID;
        int valueDataType = inMsg.readUnsignedByte();
        answerLog << " valueDataType=" << valueDataType;
        readReportAndUpdateTypeDependent(cmdID, variableID, objectID, inMsg, valueDataType);
    } catch (tcpip::SocketException& e) {
        std::stringstream msg;
        msg << "Error while receiving command: " << e.what();
        errorMsg(msg);
        return;
    }
}


void
NetsimTraciClient::commandSetValue(int domID, int varID, const std::string& objID, std::ifstream& defFile) {
    std::stringstream msg;
    tcpip::Storage inMsg, tmp;
    int dataLength = setValueTypeDependant(tmp, defFile, msg);
    std::string msgS = msg.str();
    if (msgS != "") {
        errorMsg(msg);
    }
    send_commandSetValue(domID, varID, objID, tmp);
    answerLog << std::endl << "-> Command sent: <SetValue>:" << std::endl
              << "  domID=" << domID << " varID=" << varID
              << " objID=" << objID << std::endl;
    try {
        std::string acknowledgement;
        check_resultState(inMsg, domID, false, &acknowledgement);
        answerLog << acknowledgement << std::endl;
    } catch (tcpip::SocketException& e) {
        answerLog << e.what() << std::endl;
    }
}

void NetsimTraciClient::commandSetValueVehicleStateTable() {
    tcpip::Storage tmp, inMsg;
    int cmdId = CMD_SET_VEHICLE_STATE_TABLE;
    int varId = VAR_SPEED;
    std::string objId = "VST0";

    int listCnt = m_ptrVehStateTbl->getTableListCount();
    int itemNumber = listCnt + m_ptrVehStateTbl->getTableListItemCount();
    std::cout << "listCnt: " << listCnt << " itemNumber: " << itemNumber << std::endl;

    tmp.writeUnsignedByte(TYPE_COMPOUND);
    tmp.writeInt(itemNumber);
    int length = 1 + 4;

    for (int i = 0; i < listCnt; ++i)
        {
        std::string vehId = m_ptrVehStateTbl->getReceiverVehicleIdAt(i);

        tmp.writeUnsignedByte(TYPE_STRING);
        tmp.writeString(vehId);
        length += 1 + 4 + vehId.length();

        std::vector<MSVehicleStateTable::VehicleState> listItem =
                m_ptrVehStateTbl->getSenderVehicleListAt(i);
        for(int j = 0; j < listItem.size(); j++)
            {
            tmp.writeUnsignedByte(TYPE_COMPOUND);
            tmp.writeInt(2);
            length += 1 + 4;

            tmp.writeUnsignedByte(TYPE_STRING);
            tmp.writeString(listItem.at(j).Id);
            length += 1 + 4 + vehId.length();

            tmp.writeUnsignedByte(TYPE_DOUBLE);
            tmp.writeDouble(listItem.at(j).speed);
            length += 1 + 8;
            }
        }

    send_commandSetValue(cmdId, varId, objId, tmp);

    answerLog << std::endl << "-> Command sent: <SetValue>:" << std::endl
              << "  domID=" << cmdId << " varID=" << varId
              << " objID=" << objId << std::endl;
    try {
        std::string acknowledgement;
        check_resultState(inMsg, cmdId, false, &acknowledgement);
        answerLog << acknowledgement << std::endl;
    } catch (tcpip::SocketException& e) {
        answerLog << e.what() << std::endl;
    }
}

void
NetsimTraciClient::commandSubscribeObjectVariable(int domID, const std::string& objID,
        int beginTime, int endTime, const std::vector<int>& vars) {

    send_commandSubscribeObjectVariable(domID, objID, beginTime, endTime, vars);
    answerLog << std::endl << "-> Command sent: <SubscribeVariable>:" << std::endl
              << "  domID=" << domID << " objID=" << objID << " with " << vars.size() << " variables" << std::endl;
    tcpip::Storage inMsg;
    try {
        std::string acknowledgement;
        check_resultState(inMsg, domID, false, &acknowledgement);
        answerLog << acknowledgement << std::endl;
        validateSubscription(inMsg);
    } catch (tcpip::SocketException& e) {
        answerLog << e.what() << std::endl;
    }
}


void
NetsimTraciClient::commandSubscribeContextVariable(int domID, const std::string& objID, int beginTime, int endTime,
        int domain, SUMOReal range, int varNo, std::ifstream& defFile) {
    std::vector<int> vars;
    for (int i = 0; i < varNo; ++i) {
        int var;
        defFile >> var;
        // variable id
        vars.push_back(var);
    }
    send_commandSubscribeObjectContext(domID, objID, beginTime, endTime, domain, range, vars);
    answerLog << std::endl << "-> Command sent: <SubscribeContext>:" << std::endl
              << "  domID=" << domID << " objID=" << objID << " domain=" << domain << " range=" << range
              << " with " << varNo << " variables" << std::endl;
    tcpip::Storage inMsg;
    try {
        std::string acknowledgement;
        check_resultState(inMsg, domID, false, &acknowledgement);
        answerLog << acknowledgement << std::endl;
        validateSubscription(inMsg);
    } catch (tcpip::SocketException& e) {
        answerLog << e.what() << std::endl;
    }
}


bool
NetsimTraciClient::validateSimulationStep2(tcpip::Storage& inMsg) {
    try {
        int noSubscriptions = inMsg.readInt();
        for (int s = 0; s < noSubscriptions; ++s) {
            if (!validateSubscription(inMsg)) {
                return false;
            }
        }
    } catch (std::invalid_argument& e) {
        answerLog << "#Error while reading message:" << e.what() << std::endl;
        return false;
    }
    return true;
}


bool
NetsimTraciClient::validateSubscription(tcpip::Storage& inMsg) {
    try {
        int length = inMsg.readUnsignedByte();
        if (length == 0) {
            length = inMsg.readInt();
        }
        int cmdId = inMsg.readUnsignedByte();
        if (cmdId >= RESPONSE_SUBSCRIBE_INDUCTIONLOOP_VARIABLE && cmdId <= RESPONSE_SUBSCRIBE_GUI_VARIABLE) {
            answerLog << "  CommandID=" << cmdId;
            std::string objId = inMsg.readString();
            answerLog << "  ObjectID=" << objId;
            unsigned int varNo = inMsg.readUnsignedByte();
            answerLog << "  #variables=" << varNo << std::endl;
            for (unsigned int i = 0; i < varNo; ++i) {
                int varId = inMsg.readUnsignedByte();
                answerLog << "      VariableID=" << varId;
                bool ok = inMsg.readUnsignedByte() == RTYPE_OK;
                answerLog << "      ok=" << ok;
                int valueDataType = inMsg.readUnsignedByte();
                answerLog << " valueDataType=" << valueDataType;
                readReportAndUpdateTypeDependent(cmdId, varId, objId, inMsg, valueDataType);
            }
        } else if (cmdId >= RESPONSE_SUBSCRIBE_INDUCTIONLOOP_CONTEXT && cmdId <= RESPONSE_SUBSCRIBE_GUI_CONTEXT) {
            answerLog << "  CommandID=" << cmdId;
            answerLog << "  ObjectID=" << inMsg.readString();
            answerLog << "  Domain=" << inMsg.readUnsignedByte();
            unsigned int varNo = inMsg.readUnsignedByte();
            answerLog << "  #variables=" << varNo << std::endl;
            unsigned int objNo = inMsg.readInt();
            answerLog << "  #objects=" << objNo << std::endl;
            for (unsigned int j = 0; j < objNo; ++j) {
                answerLog << "   ObjectID=" << inMsg.readString() << std::endl;
                for (unsigned int i = 0; i < varNo; ++i) {
                    answerLog << "      VariableID=" << inMsg.readUnsignedByte();
                    bool ok = inMsg.readUnsignedByte() == RTYPE_OK;
                    answerLog << "      ok=" << ok;
                    int valueDataType = inMsg.readUnsignedByte();
                    answerLog << " valueDataType=" << valueDataType;
                    // Rudhir
                    // Need to understand and fix the arguments
                    // readReportAndUpdateTypeDependent(inMsg, valueDataType);
                }
            }
        } else {
            answerLog << "#Error: received response with command id: " << cmdId << " but expected a subscription response (0xe0-0xef / 0x90-0x9f)" << std::endl;
            return false;
        }
    } catch (std::invalid_argument& e) {
        answerLog << "#Error while reading message:" << e.what() << std::endl;
        return false;
    }
    return true;
}







// ---------- Conversion helper
int
NetsimTraciClient::setValueTypeDependant(tcpip::Storage& into, std::ifstream& defFile, std::stringstream& msg) {
    std::string dataTypeS, valueS;
    defFile >> dataTypeS;
    if (dataTypeS == "<airDist>") {
        into.writeUnsignedByte(REQUEST_AIRDIST);
        return 1;
    } else if (dataTypeS == "<drivingDist>") {
        into.writeUnsignedByte(REQUEST_DRIVINGDIST);
        return 1;
    } else if (dataTypeS == "<objSubscription>") {
        int beginTime, endTime, numVars;
        defFile >> beginTime >> endTime >> numVars;
        into.writeInt(beginTime);
        into.writeInt(endTime);
        into.writeInt(numVars);
        for (int i = 0; i < numVars; ++i) {
            int var;
            defFile >> var;
            into.writeUnsignedByte(var);
        }
        return 4 + 4 + 4 + numVars;
    }
    defFile >> valueS;
    if (dataTypeS == "<int>") {
        into.writeUnsignedByte(TYPE_INTEGER);
        into.writeInt(atoi(valueS.c_str()));
        return 4 + 1;
    } else if (dataTypeS == "<byte>") {
        into.writeUnsignedByte(TYPE_BYTE);
        into.writeByte(atoi(valueS.c_str()));
        return 1 + 1;
    }  else if (dataTypeS == "<ubyte>") {
        into.writeUnsignedByte(TYPE_UBYTE);
        into.writeUnsignedByte(atoi(valueS.c_str()));
        return 1 + 1;
    } else if (dataTypeS == "<float>") {
        into.writeUnsignedByte(TYPE_FLOAT);
        into.writeFloat(float(atof(valueS.c_str())));
        return 4 + 1;
    } else if (dataTypeS == "<double>") {
        into.writeUnsignedByte(TYPE_DOUBLE);
        into.writeDouble(atof(valueS.c_str()));
        return 8 + 1;
    } else if (dataTypeS == "<string>") {
        into.writeUnsignedByte(TYPE_STRING);
        into.writeString(valueS);
        return 4 + 1 + (int) valueS.length();
    } else if (dataTypeS == "<string*>") {
        std::vector<std::string> slValue;
        int number = atoi(valueS.c_str());
        int length = 1 + 4;
        for (int i = 0; i < number; ++i) {
            std::string tmp;
            defFile >> tmp;
            slValue.push_back(tmp);
            length += 4 + int(tmp.length());
        }
        into.writeUnsignedByte(TYPE_STRINGLIST);
        into.writeStringList(slValue);
        return length;
    } else if (dataTypeS == "<compound>") {
        int number = atoi(valueS.c_str());
        into.writeUnsignedByte(TYPE_COMPOUND);
        into.writeInt(number);
        int length = 1 + 4;
        for (int i = 0; i < number; ++i) {
            length += setValueTypeDependant(into, defFile, msg);
        }
        return length;
    } else if (dataTypeS == "<color>") {
        into.writeUnsignedByte(TYPE_COLOR);
        into.writeUnsignedByte(atoi(valueS.c_str()));
        for (int i = 0; i < 3; ++i) {
            defFile >> valueS;
            into.writeUnsignedByte(atoi(valueS.c_str()));
        }
        return 1 + 4;
    } else if (dataTypeS == "<position2D>") {
        into.writeUnsignedByte(POSITION_2D);
        into.writeDouble(atof(valueS.c_str()));
        defFile >> valueS;
        into.writeDouble(atof(valueS.c_str()));
        return 1 + 8 + 8;
    } else if (dataTypeS == "<position3D>") {
        into.writeUnsignedByte(POSITION_3D);
        into.writeDouble(atof(valueS.c_str()));
        defFile >> valueS;
        into.writeDouble(atof(valueS.c_str()));
        defFile >> valueS;
        into.writeDouble(atof(valueS.c_str()));
        return 1 + 8 + 8 + 8;
    } else if (dataTypeS == "<positionRoadmap>") {
        into.writeUnsignedByte(POSITION_ROADMAP);
        into.writeString(valueS);
        int length = 1 + 8 + (int) valueS.length();
        defFile >> valueS;
        into.writeDouble(atof(valueS.c_str()));
        defFile >> valueS;
        into.writeUnsignedByte(atoi(valueS.c_str()));
        return length + 4 + 1;
    } else if (dataTypeS == "<shape>") {
        into.writeUnsignedByte(TYPE_POLYGON);
        int number = atoi(valueS.c_str());
        into.writeUnsignedByte(number);
        int length = 1 + 1;
        for (int i = 0; i < number; ++i) {
            std::string x, y;
            defFile >> x >> y;
            into.writeDouble(atof(x.c_str()));
            into.writeDouble(atof(y.c_str()));
            length += 8 + 8;
        }
        return length;
    }
    msg << "## Unknown data type: " << dataTypeS;
    return 0;
}

void
NetsimTraciClient::readReportAndUpdateTypeDependent(int cmdId, int varId, std::string objId,
        tcpip::Storage& inMsg, int valueDataType)
    {
    if (valueDataType == TYPE_UBYTE) {
        int ubyte = inMsg.readUnsignedByte();
        answerLog << " Unsigned Byte Value: " << ubyte << std::endl;
    } else if (valueDataType == TYPE_BYTE) {
        int byte = inMsg.readByte();
        answerLog << " Byte value: " << byte << std::endl;
    } else if (valueDataType == TYPE_INTEGER) {
        int integer = inMsg.readInt();
        answerLog << " Int value: " << integer << std::endl;
    } else if (valueDataType == TYPE_FLOAT) {
        float floatv = inMsg.readFloat();
        if (floatv < 0.1 && floatv > 0) {
            answerLog.setf(std::ios::scientific, std::ios::floatfield);
        }
        answerLog << " float value: " << floatv << std::endl;
        answerLog.setf(std::ios::fixed , std::ios::floatfield); // use decimal format
        answerLog.setf(std::ios::showpoint); // print decimal point
        answerLog << std::setprecision(2);
    } else if (valueDataType == TYPE_DOUBLE) {
        double doublev = inMsg.readDouble();
        answerLog << " Double value: " << doublev << std::endl;

        // Check for cmdId and varId, then update the required table
        if((cmdId == RESPONSE_SUBSCRIBE_VEHICLE_VARIABLE) &&
                (varId == VAR_SPEED))
            {
            // After you receive speed, it is confirmed that the
            // subscription was successful.
            //m_subscribedVehicleList.push_back(objId);

            // Update with proper speed value obtained from the message
            m_vehicleSpeedList[objId] = doublev;
            }

    } else if (valueDataType == TYPE_BOUNDINGBOX) {
        SUMOReal lowerLeftX = inMsg.readDouble();
        SUMOReal lowerLeftY = inMsg.readDouble();
        SUMOReal upperRightX = inMsg.readDouble();
        SUMOReal upperRightY = inMsg.readDouble();
        answerLog << " BoundaryBoxValue: lowerLeft x=" << lowerLeftX
                  << " y=" << lowerLeftY << " upperRight x=" << upperRightX
                  << " y=" << upperRightY << std::endl;
    } else if (valueDataType == TYPE_POLYGON) {
        int length = inMsg.readUnsignedByte();
        answerLog << " PolygonValue: ";
        for (int i = 0; i < length; i++) {
            SUMOReal x = inMsg.readDouble();
            SUMOReal y = inMsg.readDouble();
            answerLog << "(" << x << "," << y << ") ";
        }
        answerLog << std::endl;
    } else if (valueDataType == POSITION_3D) {
        SUMOReal x = inMsg.readDouble();
        SUMOReal y = inMsg.readDouble();
        SUMOReal z = inMsg.readDouble();
        answerLog << " Position3DValue: " << std::endl;
        answerLog << " x: " << x << " y: " << y
                  << " z: " << z << std::endl;
    } else if (valueDataType == POSITION_ROADMAP) {
        std::string roadId = inMsg.readString();
        SUMOReal pos = inMsg.readDouble();
        int laneId = inMsg.readUnsignedByte();
        answerLog << " RoadMapPositionValue: roadId=" << roadId
                  << " pos=" << pos
                  << " laneId=" << laneId << std::endl;
    } else if (valueDataType == TYPE_TLPHASELIST) {
        int length = inMsg.readUnsignedByte();
        answerLog << " TLPhaseListValue: length=" << length << std::endl;
        for (int i = 0; i < length; i++) {
            std::string pred = inMsg.readString();
            std::string succ = inMsg.readString();
            int phase = inMsg.readUnsignedByte();
            answerLog << " precRoad=" << pred << " succRoad=" << succ
                      << " phase=";
            switch (phase) {
                case TLPHASE_RED:
                    answerLog << "red" << std::endl;
                    break;
                case TLPHASE_YELLOW:
                    answerLog << "yellow" << std::endl;
                    break;
                case TLPHASE_GREEN:
                    answerLog << "green" << std::endl;
                    break;
                default:
                    answerLog << "#Error: unknown phase value" << (int)phase << std::endl;
                    return;
            }
        }
    } else if (valueDataType == TYPE_STRING) {
        std::string s = inMsg.readString();
        answerLog << " string value: " << s << std::endl;
    } else if (valueDataType == TYPE_STRINGLIST) {
        std::vector<std::string> s = inMsg.readStringList();
        answerLog << " string list value: [ " << std::endl;
        for (std::vector<std::string>::iterator i = s.begin(); i != s.end(); ++i) {
            if (i != s.begin()) {
                answerLog << ", ";
            }
            answerLog << '"' << *i << '"';
        }
        answerLog << " ]" << std::endl;

        if((cmdId == RESPONSE_SUBSCRIBE_VEHICLE_VARIABLE) &&
                (varId == ID_LIST))
            {
            for (std::vector<std::string>::iterator i = s.begin(); i != s.end(); ++i)
                {
                // List of vehicles in simulation.
                //m_vehicleSpeedList[*i] = 0.0;
                m_vehicleList.push_back(*i);
                }
            }
    } else if (valueDataType == TYPE_COMPOUND) {
        int no = inMsg.readInt();
        answerLog << " compound value with " << no << " members: [ " << std::endl;
        for (int i = 0; i < no; ++i) {
            int currentValueDataType = inMsg.readUnsignedByte();
            answerLog << " valueDataType=" << currentValueDataType;
            readReportAndUpdateTypeDependent(cmdId, varId, objId, inMsg, currentValueDataType);
        }
        answerLog << " ]" << std::endl;
    } else if (valueDataType == POSITION_2D) {
        SUMOReal xv = inMsg.readDouble();
        SUMOReal yv = inMsg.readDouble();
        answerLog << " position value: (" << xv << "," << yv << ")" << std::endl;
    } else if (valueDataType == TYPE_COLOR) {
        int r = inMsg.readUnsignedByte();
        int g = inMsg.readUnsignedByte();
        int b = inMsg.readUnsignedByte();
        int a = inMsg.readUnsignedByte();
        answerLog << " color value: (" << r << "," << g << "," << b << "," << a << ")" << std::endl;
    } else {
        answerLog << "#Error: unknown valueDataType!" << std::endl;
    }
    }

void NetsimTraciClient::commandSubscribeIdList(int currTimeInSec, int endTimeInSec)
    {
    std::vector<int> idListVars;
    idListVars.push_back(ID_LIST);

    // Send subscribe command ID_LIST
    commandSubscribeObjectVariable(CMD_SUBSCRIBE_VEHICLE_VARIABLE,"dummyObjId",
            TIME2STEPS(currTimeInSec), TIME2STEPS(endTimeInSec), idListVars);
    }

void NetsimTraciClient::commandSubscribeSpeedAndPos(int currTimeInSec, int endTimeInSec)
    {
    std::vector<int> getVehVars;
    getVehVars.push_back(VAR_SPEED);
    getVehVars.push_back(VAR_POSITION);

    // m_vehicleSpeedList contains all vehicles subscribed
    // for command VAR_SPEED and VAR_POSITION. Check against this
    // list to find out new subscriptions required.
    for(std::vector<std::string>::iterator iter = m_vehicleList.begin();
                    iter != m_vehicleList.end();iter++)
        {
        VehicleSpeedList::iterator vIter = m_vehicleSpeedList.find(*iter);
        if (vIter == m_vehicleSpeedList.end())
            {
            // New vehicle added. Send subscribe command for this vehicle.
            answerLog << std::endl << "Vehicle added " << *iter;
            commandSubscribeObjectVariable(CMD_SUBSCRIBE_VEHICLE_VARIABLE,
                                           *iter,
                                           TIME2STEPS(currTimeInSec),
                                           TIME2STEPS(endTimeInSec),
                                           getVehVars);
            }
         }
    }

void NetsimTraciClient::clearActiveLists()
    {
    m_vehicleList.clear();
    m_vehicleSpeedList.erase(m_vehicleSpeedList.begin(), m_vehicleSpeedList.end());
    }

void NetsimTraciClient::displayActiveLists()
    {
    std::cout << std::endl;
    std::cout << ">>>> m_vehicleList: <<<<" << std::endl;
    for(std::vector<std::string>::iterator iter=m_vehicleList.begin();
            iter != m_vehicleList.end();iter++)
        {
        std::cout << *iter << " ";
        }
    std::cout << std::endl;

    std::cout << ">>>> m_vehicleSpeedList: <<<<" << std::endl;
    for(VehicleSpeedList::iterator iter = m_vehicleSpeedList.begin();
            iter != m_vehicleSpeedList.end();iter++)
        {
        std::cout << iter->first << ", " << iter->second << " ";
        }
    std::cout << std::endl;
    }

void NetsimTraciClient::writeResult() {
    time_t seconds;
    tm* locTime;
    std::ofstream outFile(outputFileName.c_str());
    if (!outFile) {
        std::cerr << "Unable to write result file" << std::endl;
    }
    time(&seconds);
    locTime = localtime(&seconds);
    outFile << "NetsimTraciClient output file. Date: " << asctime(locTime) << std::endl;
    outFile << answerLog.str();
    outFile.close();
}

void NetsimTraciClient::errorMsg(std::stringstream& msg) {
    std::cerr << msg.str() << std::endl;
    answerLog << "----" << std::endl << msg.str() << std::endl;
}

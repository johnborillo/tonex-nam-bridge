// Shared constants between the NAM Bridge driver and its controllers (CLI, menu-bar app).
// The Swift app mirrors these values in app/Sources/DriverClient.swift; keep them in sync.
#pragma once

#include <CoreAudio/AudioHardwareBase.h>

namespace nambridge::protocol {

constexpr const char* kDeviceUID = "NAMBridge_Device_UID";
constexpr const char* kDeviceName = "NAM Bridge";

// Custom properties on the device object (global scope, main element).
//
// 'nbMD' CFDictionary set: { json: <full text of a .nam file>, fileName: string } loads it;
//                         { unload: true } unloads. get: { fileName } of the loaded model.
// 'nbCF' CFDictionary set/get: { inputGainDb: number, outputGainDb: number, inputSelect: 0|1|2 }
// 'nbST' CFDictionary get: { loaded, modelName, architecture, modelRate, hostRate, resampling,
//                            inputGainDb, outputGainDb, inputSelect, inPeakDb, outPeakDb,
//                            running, lastError, hasLoudness, loudnessDb, version }
constexpr AudioObjectPropertySelector kPropModel = 'nbMD';
constexpr AudioObjectPropertySelector kPropConfig = 'nbCF';
constexpr AudioObjectPropertySelector kPropStatus = 'nbST';

} // namespace nambridge::protocol

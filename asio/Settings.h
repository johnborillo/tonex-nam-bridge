// Settings: per-user persistence for the NAM Bridge ASIO driver.
//
//   HKCU\Software\tonex-nam-bridge\NAMBridgeASIO
//     ModelPath     REG_SZ  full path of the .nam file ("" = none)
//     InputGainDb   REG_SZ  decimal dB, locale-independent ("-3.5")
//     OutputGainDb  REG_SZ  decimal dB
//     InputSelect   DWORD   0 = left (out 1), 1 = right (out 2), 2 = (L+R)/2
//
// Never touched from the clock thread.
#pragma once

#include <filesystem>
#include <string>

namespace nambridge::asio {

inline constexpr wchar_t kSettingsKey[] = L"Software\\tonex-nam-bridge\\NAMBridgeASIO";

struct Settings
{
    std::filesystem::path modelPath;
    float inputGainDb = 0.0f;
    float outputGainDb = 0.0f;
    int inputSelect = 0;

    // Returns false if the key does not exist (fields keep their defaults).
    bool Load();
    bool Save() const;
    static bool Delete();
};

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

} // namespace nambridge::asio

#include "Settings.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <system_error>

namespace nambridge::asio {

namespace {

bool ReadString(HKEY key, const wchar_t* name, std::wstring& out)
{
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ)
        return false;
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes) != ERROR_SUCCESS)
        return false;
    buf.resize(bytes / sizeof(wchar_t));
    while (!buf.empty() && buf.back() == L'\0')
        buf.pop_back();
    out = buf;
    return true;
}

bool WriteString(HKEY key, const wchar_t* name, const std::wstring& v)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                          static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// Locale-independent: a host may have called setlocale() with a decimal comma.
bool ReadFloat(HKEY key, const wchar_t* name, float& out)
{
    std::wstring w;
    if (!ReadString(key, name, w))
        return false;
    const std::string s = ToUtf8(w);
    float v = 0.0f;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc())
        return false;
    out = v;
    return true;
}

bool WriteFloat(HKEY key, const wchar_t* name, float v)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return WriteString(key, name, FromUtf8(std::string(buf, r.ptr)));
}

} // namespace

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool Settings::Load()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    std::wstring path;
    if (ReadString(key, L"ModelPath", path))
        modelPath = path;
    ReadFloat(key, L"InputGainDb", inputGainDb);
    ReadFloat(key, L"OutputGainDb", outputGainDb);
    DWORD sel = 0, type = 0, bytes = sizeof sel;
    if (RegQueryValueExW(key, L"InputSelect", nullptr, &type, reinterpret_cast<BYTE*>(&sel), &bytes) ==
            ERROR_SUCCESS &&
        type == REG_DWORD)
        inputSelect = static_cast<int>(std::min<DWORD>(sel, 2));
    RegCloseKey(key);
    return true;
}

bool Settings::Save() const
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    bool ok = WriteString(key, L"ModelPath", modelPath.wstring());
    ok = WriteFloat(key, L"InputGainDb", inputGainDb) && ok;
    ok = WriteFloat(key, L"OutputGainDb", outputGainDb) && ok;
    const DWORD sel = static_cast<DWORD>(inputSelect);
    ok = RegSetValueExW(key, L"InputSelect", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&sel), sizeof sel) ==
             ERROR_SUCCESS &&
         ok;
    RegCloseKey(key);
    return ok;
}

bool Settings::Delete()
{
    const LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

} // namespace nambridge::asio

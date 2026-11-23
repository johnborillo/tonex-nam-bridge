// In-process COM server for the NAM Bridge ASIO driver: class factory, DLL entry points and
// registration. Exports (exports.def): DllGetClassObject, DllCanUnloadNow, DllRegisterServer,
// DllUnregisterServer.

#include <windows.h>
#include <objbase.h>
#include <olectl.h>

#include <atomic>
#include <new>
#include <string>

#include "AsioDriver.h"
#include "Module.h"

namespace nambridge::asio {

HMODULE gModule = nullptr;
std::atomic<long> gObjects{0};
std::atomic<long> gLocks{0};

namespace {

constexpr wchar_t kAsioKey[] = L"SOFTWARE\\ASIO\\NAM Bridge";
constexpr wchar_t kDescription[] = L"NAM Bridge";

class ClassFactory final : public IClassFactory
{
public:
    ClassFactory() { gObjects++; }
    ~ClassFactory() { gObjects--; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --refs_;
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        auto* driver = new (std::nothrow) AsioDriver();
        if (!driver)
            return E_OUTOFMEMORY;
        const HRESULT hr = driver->QueryInterface(riid, ppv); // riid may be the CLSID (ASIO quirk)
        driver->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock)
            gLocks++;
        else
            gLocks--;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

std::wstring ClsidString()
{
    wchar_t buf[64] = {};
    StringFromGUID2(kClsid, buf, 64);
    return buf;
}

bool SetString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS st = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool DeleteTree(HKEY root, const std::wstring& path)
{
    const LSTATUS st = RegDeleteTreeW(root, path.c_str());
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND)
        return false;
    RegDeleteKeyW(root, path.c_str()); // RegDeleteTree leaves the key itself
    return true;
}

} // namespace

} // namespace nambridge::asio

using namespace nambridge::asio;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        gModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (rclsid != kClsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory)
        return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return gObjects.load() == 0 && gLocks.load() == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(gModule, path, static_cast<DWORD>(std::size(path)));
    if (n == 0 || n >= std::size(path))
        return SELFREG_E_CLASS;
    const std::wstring clsidKey = L"CLSID\\" + ClsidString();
    const bool ok = SetString(HKEY_CLASSES_ROOT, clsidKey, nullptr, kDescription) &&
                    SetString(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32", nullptr, path) &&
                    SetString(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
                    SetString(HKEY_LOCAL_MACHINE, kAsioKey, L"CLSID", ClsidString()) &&
                    SetString(HKEY_LOCAL_MACHINE, kAsioKey, L"Description", kDescription);
    return ok ? S_OK : SELFREG_E_CLASS; // usually: not run elevated
}

STDAPI DllUnregisterServer()
{
    const bool a = DeleteTree(HKEY_CLASSES_ROOT, L"CLSID\\" + ClsidString());
    const bool b = DeleteTree(HKEY_LOCAL_MACHINE, kAsioKey);
    return a && b ? S_OK : SELFREG_E_CLASS;
}

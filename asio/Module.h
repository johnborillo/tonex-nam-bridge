// Shared module state for the ASIO driver DLL.
#pragma once

#include <windows.h>

#include <atomic>

namespace nambridge::asio {

class AsioDriver;

extern HMODULE gModule;            // this DLL (set in DllMain)
extern std::atomic<long> gObjects; // live COM objects (drivers + class factories)
extern std::atomic<long> gLocks;   // IClassFactory::LockServer count

// Modal settings dialog. Runs on the caller's (host UI) thread.
void ShowControlPanel(AsioDriver& driver, HWND parent);

} // namespace nambridge::asio

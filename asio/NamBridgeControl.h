// INamBridgeControl: a private COM interface on the NAM Bridge ASIO driver object, used by
// tests/asio_harness.cpp to drive model loads from another thread while audio runs.
// Not registered anywhere and not exported: reach it with QueryInterface on the driver.
// Unlike the control panel, it does not persist anything to the registry.
#pragma once

#include <objbase.h>

struct NamBridgeStatus
{
    BOOL modelLoaded;
    BOOL resampling;
    BOOL running;
    double sampleRate;   // device (host-set) rate
    double modelRate;
    long bufferSize;     // 0 before createBuffers
    unsigned long long ticks;    // clock ticks since start()
    unsigned long long overruns; // times the clock woke > 2 periods late and resynchronized
    double processSeconds;       // wall time spent in NamEngine::Process since start()
};

MIDL_INTERFACE("CCB9254F-A8F6-4A52-B7AF-B574CAE24E58")
INamBridgeControl : public IUnknown
{
    // Load a .nam file (UTF-16 path). E_FAIL on error; the previous model stays active.
    virtual HRESULT STDMETHODCALLTYPE LoadModel(const wchar_t* path) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnloadModel() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(NamBridgeStatus* status) = 0;
};

// Tiny CoreFoundation helpers shared by the driver and the CLI.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <string>

namespace nambridge::cf {

inline std::string ToString(CFStringRef s)
{
    if (!s)
        return {};
    if (const char* p = CFStringGetCStringPtr(s, kCFStringEncodingUTF8))
        return p;
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8))
        return {};
    out.resize(strlen(out.c_str()));
    return out;
}

inline CFStringRef MakeString(const std::string& s)
{
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()),
                                   static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
}

inline void SetString(CFMutableDictionaryRef d, const char* key, const std::string& value)
{
    CFStringRef k = MakeString(key), v = MakeString(value);
    CFDictionarySetValue(d, k, v);
    CFRelease(k);
    CFRelease(v);
}

inline void SetNumber(CFMutableDictionaryRef d, const char* key, double value)
{
    CFStringRef k = MakeString(key);
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
    CFDictionarySetValue(d, k, v);
    CFRelease(k);
    CFRelease(v);
}

inline void SetBool(CFMutableDictionaryRef d, const char* key, bool value)
{
    CFStringRef k = MakeString(key);
    CFDictionarySetValue(d, k, value ? kCFBooleanTrue : kCFBooleanFalse);
    CFRelease(k);
}

inline CFTypeRef Get(CFDictionaryRef d, const char* key)
{
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID())
        return nullptr;
    CFStringRef k = MakeString(key);
    CFTypeRef v = CFDictionaryGetValue(d, k);
    CFRelease(k);
    return v;
}

inline bool GetNumber(CFDictionaryRef d, const char* key, double& out)
{
    CFTypeRef v = Get(d, key);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        return CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberDoubleType, &out);
    if (v && CFGetTypeID(v) == CFBooleanGetTypeID())
    {
        out = CFBooleanGetValue(static_cast<CFBooleanRef>(v)) ? 1.0 : 0.0;
        return true;
    }
    return false;
}

inline bool GetString(CFDictionaryRef d, const char* key, std::string& out)
{
    CFTypeRef v = Get(d, key);
    if (!v || CFGetTypeID(v) != CFStringGetTypeID())
        return false;
    out = ToString(static_cast<CFStringRef>(v));
    return true;
}

inline CFMutableDictionaryRef MakeDict()
{
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

} // namespace nambridge::cf

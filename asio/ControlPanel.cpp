// Control panel: a modal Win32 dialog opened by IASIO::controlPanel() on the host's UI
// thread. Audio keeps running on the clock thread. Every change applies immediately and is
// saved to the registry. The dialog never changes the sample rate or buffer size.
//
// Built from an in-memory template (no .rc): controls are laid out in dialog units and
// created in WM_INITDIALOG.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "AsioDriver.h"
#include "Module.h"

namespace nambridge::asio {

namespace {

enum : int
{
    IDC_NAME = 1001,
    IDC_RATES,
    IDC_PATH,
    IDC_ERROR,
    IDC_LOAD,
    IDC_UNLOAD,
    IDC_INGAIN,
    IDC_INGAIN_VAL,
    IDC_OUTGAIN,
    IDC_OUTGAIN_VAL,
    IDC_SOURCE,
    IDC_INMETER,
    IDC_INMETER_VAL,
    IDC_OUTMETER,
    IDC_OUTMETER_VAL,
    IDC_STATUS,
};

constexpr UINT_PTR kMeterTimer = 1;
constexpr int kGainSteps = 96; // -24 .. +24 dB in 0.5 dB steps
constexpr int kMeterFloorDb = -60;

int GainToPos(float db) { return std::clamp(static_cast<int>(std::lround(db * 2.0f)) + kGainSteps / 2, 0, kGainSteps); }
float PosToGain(int pos) { return static_cast<float>(pos - kGainSteps / 2) * 0.5f; }

AsioDriver* DriverOf(HWND dlg) { return reinterpret_cast<AsioDriver*>(GetWindowLongPtrW(dlg, GWLP_USERDATA)); }

HWND Make(HWND dlg, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    RECT r = {x, y, x + w, y + h};
    MapDialogRect(dlg, &r);
    HWND wnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                               r.bottom - r.top, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gModule,
                               nullptr);
    SendMessageW(wnd, WM_SETFONT, static_cast<WPARAM>(SendMessageW(dlg, WM_GETFONT, 0, 0)), TRUE);
    return wnd;
}

void SetText(HWND dlg, int id, const std::wstring& s) { SetDlgItemTextW(dlg, id, s.c_str()); }

std::wstring Format(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring DbText(float lin)
{
    if (lin <= 1e-9f)
        return L"-inf dBFS";
    return Format(L"%.1f dBFS", 20.0f * std::log10(lin));
}

void RefreshModel(HWND dlg, AsioDriver& drv)
{
    const ModelInfo info = drv.Engine().Info();
    if (info.loaded)
    {
        SetText(dlg, IDC_NAME, FromUtf8(info.name) + L"  [" + FromUtf8(info.architecture) + L"]");
        SetText(dlg, IDC_RATES, Format(L"Model %.0f Hz, host %.0f Hz%s", info.modelRate, info.hostRate,
                                       info.resampling ? L"  (resampling)" : L""));
    }
    else
    {
        SetText(dlg, IDC_NAME, L"No model loaded (input is silent)");
        SetText(dlg, IDC_RATES, Format(L"Host %.0f Hz", info.hostRate));
    }
    SetText(dlg, IDC_PATH, drv.Config().modelPath.wstring());
    const std::string err = drv.LastError();
    SetText(dlg, IDC_ERROR, err.empty() ? L"" : L"Error: " + FromUtf8(err));
    EnableWindow(GetDlgItem(dlg, IDC_UNLOAD), info.loaded);
}

void RefreshMeters(HWND dlg, AsioDriver& drv)
{
    float in = 0.0f, out = 0.0f;
    drv.Engine().TakePeaks(in, out);
    auto meter = [&](int bar, int label, float lin) {
        const float db = lin > 1e-9f ? 20.0f * std::log10(lin) : static_cast<float>(kMeterFloorDb);
        const int pos = static_cast<int>(std::lround((std::clamp(db, static_cast<float>(kMeterFloorDb), 0.0f) -
                                                      kMeterFloorDb) *
                                                     10.0f));
        SendDlgItemMessageW(dlg, bar, PBM_SETPOS, static_cast<WPARAM>(pos), 0);
        SetText(dlg, label, DbText(lin));
    };
    meter(IDC_INMETER, IDC_INMETER_VAL, in);
    meter(IDC_OUTMETER, IDC_OUTMETER_VAL, out);
    if (drv.Running())
        SetText(dlg, IDC_STATUS, Format(L"Running at %.0f Hz, buffer %ld. DSP load %.0f%%, clock overruns %llu",
                                        drv.SampleRate(), drv.BufferSize(), 100.0 * drv.DspLoad(),
                                        static_cast<unsigned long long>(drv.Overruns())));
    else
        SetText(dlg, IDC_STATUS, Format(L"Idle (device rate %.0f Hz)", drv.SampleRate()));
}

void SetGainLabel(HWND dlg, int id, float db) { SetText(dlg, id, Format(L"%+.1f dB", db)); }

void Save(HWND dlg, AsioDriver& drv)
{
    if (!drv.Config().Save())
        SetText(dlg, IDC_ERROR, L"Error: could not save settings to the registry");
}

void Create(HWND dlg, AsioDriver& drv)
{
    Make(dlg, L"BUTTON", L"Model", BS_GROUPBOX, 7, 5, 246, 82, -1);
    Make(dlg, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 14, 17, 232, 9, IDC_NAME);
    Make(dlg, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 14, 28, 232, 9, IDC_RATES);
    Make(dlg, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 14, 39, 232, 9, IDC_PATH);
    Make(dlg, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 14, 50, 232, 9, IDC_ERROR);
    Make(dlg, L"BUTTON", L"&Load .nam...", BS_PUSHBUTTON | WS_TABSTOP, 14, 65, 64, 14, IDC_LOAD);
    Make(dlg, L"BUTTON", L"&Unload", BS_PUSHBUTTON | WS_TABSTOP, 84, 65, 50, 14, IDC_UNLOAD);

    Make(dlg, L"BUTTON", L"Levels", BS_GROUPBOX, 7, 92, 246, 66, -1);
    Make(dlg, L"STATIC", L"Input gain", SS_LEFT, 14, 106, 50, 9, -1);
    Make(dlg, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 64, 102, 140, 16, IDC_INGAIN);
    Make(dlg, L"STATIC", L"", SS_RIGHT, 206, 106, 40, 9, IDC_INGAIN_VAL);
    Make(dlg, L"STATIC", L"Output gain", SS_LEFT, 14, 124, 50, 9, -1);
    Make(dlg, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 64, 120, 140, 16, IDC_OUTGAIN);
    Make(dlg, L"STATIC", L"", SS_RIGHT, 206, 124, 40, 9, IDC_OUTGAIN_VAL);
    Make(dlg, L"STATIC", L"Source channel", SS_LEFT, 14, 142, 50, 9, -1);
    Make(dlg, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 64, 140, 100, 60, IDC_SOURCE);

    Make(dlg, L"BUTTON", L"Meters", BS_GROUPBOX, 7, 163, 246, 50, -1);
    Make(dlg, L"STATIC", L"In", SS_LEFT, 14, 176, 20, 9, -1);
    Make(dlg, PROGRESS_CLASSW, L"", PBS_SMOOTH, 36, 175, 150, 10, IDC_INMETER);
    Make(dlg, L"STATIC", L"", SS_RIGHT, 190, 176, 56, 9, IDC_INMETER_VAL);
    Make(dlg, L"STATIC", L"Out", SS_LEFT, 14, 188, 20, 9, -1);
    Make(dlg, PROGRESS_CLASSW, L"", PBS_SMOOTH, 36, 187, 150, 10, IDC_OUTMETER);
    Make(dlg, L"STATIC", L"", SS_RIGHT, 190, 188, 56, 9, IDC_OUTMETER_VAL);
    Make(dlg, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 14, 200, 232, 9, IDC_STATUS);

    Make(dlg, L"STATIC", L"In ToneX Modeler use output 1 and input 1.", SS_LEFT, 7, 222, 190, 9, -1);
    Make(dlg, L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 203, 219, 50, 14, IDOK);

    for (int id : {IDC_INGAIN, IDC_OUTGAIN})
    {
        SendDlgItemMessageW(dlg, id, TBM_SETRANGE, TRUE, MAKELPARAM(0, kGainSteps));
        SendDlgItemMessageW(dlg, id, TBM_SETTICFREQ, 12, 0); // a tick every 6 dB
        SendDlgItemMessageW(dlg, id, TBM_SETPAGESIZE, 0, 2); // 1 dB per page
    }
    const float inDb = drv.Engine().InputGainDb(), outDb = drv.Engine().OutputGainDb();
    SendDlgItemMessageW(dlg, IDC_INGAIN, TBM_SETPOS, TRUE, GainToPos(inDb));
    SendDlgItemMessageW(dlg, IDC_OUTGAIN, TBM_SETPOS, TRUE, GainToPos(outDb));
    SetGainLabel(dlg, IDC_INGAIN_VAL, inDb);
    SetGainLabel(dlg, IDC_OUTGAIN_VAL, outDb);

    for (const wchar_t* s : {L"Left (output 1)", L"Right (output 2)", L"L+R mix"})
        SendDlgItemMessageW(dlg, IDC_SOURCE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendDlgItemMessageW(dlg, IDC_SOURCE, CB_SETCURSEL, static_cast<WPARAM>(drv.Config().inputSelect), 0);

    for (int id : {IDC_INMETER, IDC_OUTMETER})
        SendDlgItemMessageW(dlg, id, PBM_SETRANGE32, 0, -kMeterFloorDb * 10);

    RefreshModel(dlg, drv);
    RefreshMeters(dlg, drv);
}

void OnLoad(HWND dlg, AsioDriver& drv)
{
    std::vector<wchar_t> file(32768, L'\0');
    std::wstring dir;
    if (!drv.Config().modelPath.empty())
        dir = drv.Config().modelPath.parent_path().wstring();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"NAM captures (*.nam)\0*.nam\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = L"Load a NAM capture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn))
        return;

    const std::filesystem::path path(file.data());
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::string err;
    const bool ok = drv.LoadModelFile(path, err);
    SetCursor(old);
    if (ok)
    {
        drv.Config().modelPath = path;
        Save(dlg, drv);
    }
    else
    {
        drv.SetLastError(err);
        MessageBoxW(dlg, (L"Could not load this model. The previous model stays active.\n\n" + FromUtf8(err)).c_str(),
                    L"NAM Bridge", MB_OK | MB_ICONERROR);
    }
    RefreshModel(dlg, drv);
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        Create(dlg, *reinterpret_cast<AsioDriver*>(lp));
        SetTimer(dlg, kMeterTimer, 100, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wp == kMeterTimer)
            RefreshMeters(dlg, *DriverOf(dlg));
        return TRUE;
    case WM_HSCROLL:
    {
        AsioDriver& drv = *DriverOf(dlg);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id != IDC_INGAIN && id != IDC_OUTGAIN)
            break;
        const float db = PosToGain(static_cast<int>(SendDlgItemMessageW(dlg, id, TBM_GETPOS, 0, 0)));
        if (id == IDC_INGAIN)
        {
            drv.Engine().SetInputGainDb(db);
            drv.Config().inputGainDb = db;
            SetGainLabel(dlg, IDC_INGAIN_VAL, db);
        }
        else
        {
            drv.Engine().SetOutputGainDb(db);
            drv.Config().outputGainDb = db;
            SetGainLabel(dlg, IDC_OUTGAIN_VAL, db);
        }
        if (LOWORD(wp) != TB_THUMBTRACK) // save once the user lets go
            Save(dlg, drv);
        return TRUE;
    }
    case WM_COMMAND:
    {
        AsioDriver& drv = *DriverOf(dlg);
        switch (LOWORD(wp))
        {
        case IDC_LOAD:
            OnLoad(dlg, drv);
            return TRUE;
        case IDC_UNLOAD:
            drv.Engine().Unload();
            drv.Config().modelPath.clear();
            drv.SetLastError("");
            Save(dlg, drv);
            RefreshModel(dlg, drv);
            return TRUE;
        case IDC_SOURCE:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                const int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_SOURCE, CB_GETCURSEL, 0, 0));
                drv.SetInputSelect(sel);
                drv.Config().inputSelect = std::clamp(sel, 0, 2);
                Save(dlg, drv);
            }
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(dlg, kMeterTimer);
        break;
    }
    return FALSE;
}

std::vector<WORD> BuildTemplate(const wchar_t* title, short cx, short cy)
{
    std::vector<WORD> t;
    auto dword = [&](DWORD v) {
        t.push_back(LOWORD(v));
        t.push_back(HIWORD(v));
    };
    auto str = [&](const wchar_t* s) {
        while (*s)
            t.push_back(static_cast<WORD>(*s++));
        t.push_back(0);
    };
    dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU); // style
    dword(0);                                                                           // extended style
    t.push_back(0);                                                                     // no template items
    t.push_back(0);                                                                     // x
    t.push_back(0);                                                                     // y
    t.push_back(static_cast<WORD>(cx));
    t.push_back(static_cast<WORD>(cy));
    t.push_back(0); // no menu
    t.push_back(0); // default class
    str(title);
    t.push_back(8); // point size
    str(L"MS Shell Dlg");
    return t;
}

} // namespace

void ShowControlPanel(AsioDriver& driver, HWND parent)
{
    static const bool commonControls = [] {
        INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
        return InitCommonControlsEx(&icc) != FALSE;
    }();
    (void)commonControls;
    const auto tmpl = BuildTemplate(L"NAM Bridge", 260, 240);
    DialogBoxIndirectParamW(gModule, reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()), parent, DlgProc,
                            reinterpret_cast<LPARAM>(&driver));
}

} // namespace nambridge::asio

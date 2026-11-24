// A tiny VST2 host to try an editor without a DAW: loads the DLL, opens the editor in a window,
// simulates a few mouse actions, saves a screenshot (BMP) and prints what changed.
//   test_host.exe <plugin.dll> <screenshot.bmp>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mpc_plugin.h"   // only for the VST2 ABI types

static VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32 index, VstIntPtr, void*, float opt)
{
    if (opcode == audioMasterVersion) return 2400;
    if (opcode == audioMasterAutomate) std::printf("  host: automate param %d -> %.3f\n", index, opt);
    if (opcode == 43) std::printf("  host: begin edit %d\n", index);
    if (opcode == 44) std::printf("  host: end edit %d\n", index);
    return 0;
}

static bool saveBmp(HWND wnd, const char* path)
{
    RECT r; GetClientRect(wnd, &r);
    int w = r.right, h = r.bottom;
    HDC dc = GetDC(wnd);
    HDC mem = CreateCompatibleDC(dc);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(wnd, mem, PW_CLIENTONLY);
    FILE* f = std::fopen(path, "wb");
    if (f)
    {
        BITMAPFILEHEADER fh = {}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + (DWORD)(w * h * 4);
        std::fwrite(&fh, sizeof(fh), 1, f); std::fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        std::fwrite(bits, 4, (size_t)w * h, f); std::fclose(f);
    }
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(wnd, dc);
    return f != nullptr;
}

static void pump(int ms)
{
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end)
    {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); }
        Sleep(5);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: test_host plugin.dll shot.bmp\n"); return 2; }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { std::printf("cannot load %s (%lu)\n", argv[1], GetLastError()); return 1; }
    typedef AEffect* (*Entry)(audioMasterCallback);
    Entry entry = (Entry)GetProcAddress(dll, "VSTPluginMain");
    if (!entry) { std::printf("no VSTPluginMain\n"); return 1; }
    AEffect* e = entry(hostCallback);
    if (!e) { std::printf("effect not created\n"); return 1; }
    std::printf("params=%d hasEditor=%d\n", e->numParams, (e->flags & effFlagsHasEditor) ? 1 : 0);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);

    ERect* rect = nullptr;
    e->dispatcher(e, effEditGetRect, 0, 0, &rect, 0);
    int w = rect->right - rect->left, h = rect->bottom - rect->top;
    std::printf("editor size %d x %d\n", w, h);

    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(nullptr); wc.lpszClassName = "TestHost";
    RegisterClassA(&wc);
    RECT win = {0, 0, w, h}; AdjustWindowRect(&win, WS_OVERLAPPEDWINDOW, FALSE);
    HWND parent = CreateWindowA("TestHost", "host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, win.right - win.left, win.bottom - win.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    std::printf("effEditOpen -> %d\n", (int)e->dispatcher(e, effEditOpen, 0, 0, parent, 0));
    pump(300);
    HWND child = GetWindow(parent, GW_CHILD);
    std::printf("child window: %s\n", child ? "yes" : "NO");
    if (!child) return 1;
    saveBmp(child, argv[2]);

    // Mouse tests: a knob (drag up), a switch (click), then print the parameter values.
    auto value = [&](int i) { return e->getParameter(e, i); };
    std::vector<float> before; for (int i = 0; i < e->numParams; ++i) before.push_back(value(i));
    // second row, third column cell (Octaves knob area) in the unscaled layout: x = 16+92*1+46, y = 40+22+42
    float s = (float)GetDeviceCaps(GetDC(nullptr), LOGPIXELSX) / 96.0f; if (s < 1) s = 1;
    auto P = [&](int x, int y) { return MAKELPARAM((int)(x * s), (int)(y * s)); };
    SendMessage(child, WM_LBUTTONDOWN, MK_LBUTTON, P(16 + 92 * 1 + 46, 40 + 22 + 42));
    SendMessage(child, WM_MOUSEMOVE, MK_LBUTTON, P(16 + 92 * 1 + 46, 40 + 22 + 42 - 60));
    SendMessage(child, WM_LBUTTONUP, 0, P(16 + 92 * 1 + 46, 40 + 22 + 42 - 60));
    pump(100);
    for (int i = 0; i < e->numParams; ++i)
        if (value(i) != before[i]) { char nm[40] = {0}; e->dispatcher(e, effGetParamName, i, 0, nm, 0); std::printf("  changed: %s %.3f -> %.3f\n", nm, before[i], value(i)); }
    if (argc >= 5)
    {
        SendMessage(child, WM_LBUTTONDOWN, MK_LBUTTON, P(std::atoi(argv[3]), std::atoi(argv[4])));
        SendMessage(child, WM_LBUTTONUP, 0, P(std::atoi(argv[3]), std::atoi(argv[4])));
        pump(150);
    }
    std::string shot2 = std::string(argv[2]) + ".after.bmp";
    saveBmp(child, shot2.c_str());

    e->dispatcher(e, effEditClose, 0, 0, nullptr, 0);
    DestroyWindow(parent);
    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    std::printf("closed cleanly\n");
    return 0;
}

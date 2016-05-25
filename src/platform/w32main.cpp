//-----------------------------------------------------------------------------
// Our WinMain() functions, and Win32-specific stuff to set up our windows
// and otherwise handle our interface to the operating system. Everything
// outside platform/... should be standard C++ and gl.
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------
#include <time.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>

#include "solvespace.h"
#include "config.h"

#ifdef HAVE_SPACEWARE
#   include <si.h>
#   include <siapp.h>
#   undef uint32_t  // thanks but no thanks
#endif

HINSTANCE Instance;

HWND TextWnd;
HWND TextWndScrollBar;
HWND TextEditControl;
HGLRC TextGl;

HWND GraphicsWnd;
HGLRC GraphicsGl;
HWND GraphicsEditControl;
static struct {
    int x, y;
} LastMousePos;

HMENU SubMenus[100];
HMENU RecentOpenMenu, RecentImportMenu;

HMENU ContextMenu, ContextSubmenu;

int ClientIsSmallerBy;

HFONT FixedFont;

#ifdef HAVE_SPACEWARE
// The 6-DOF input device.
SiHdl SpaceNavigator = SI_NO_HANDLE;
#endif

//-----------------------------------------------------------------------------
// Routines to display message boxes on screen. Do our own, instead of using
// MessageBox, because that is not consistent from version to version and
// there's word wrap problems.
//-----------------------------------------------------------------------------

HWND MessageWnd, OkButton;
bool MessageDone;
const char *MessageString;

static LRESULT CALLBACK MessageProc(HWND hwnd, UINT msg, WPARAM wParam,
    LPARAM lParam)
{
    switch (msg) {
        case WM_COMMAND:
            if((HWND)lParam == OkButton && wParam == BN_CLICKED) {
                MessageDone = true;
            }
            break;

        case WM_CLOSE:
        case WM_DESTROY:
            MessageDone = true;
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            int row = 0, col = 0, i;
            SelectObject(hdc, FixedFont);
            SetTextColor(hdc, 0x000000);
            SetBkMode(hdc, TRANSPARENT);
            for(i = 0; MessageString[i]; i++) {
                if(MessageString[i] == '\n') {
                    col = 0;
                    row++;
                } else {
                    TextOutW(hdc, col*SS.TW.CHAR_WIDTH + 10,
                                  row*SS.TW.LINE_HEIGHT + 10,
                                  Widen(&(MessageString[i])).c_str(), 1);
                    col++;
                }
            }
            EndPaint(hwnd, &ps);
            break;
        }

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 1;
}

HWND CreateWindowClient(DWORD exStyle, const wchar_t *className, const wchar_t *windowName,
    DWORD style, int x, int y, int width, int height, HWND parent,
    HMENU menu, HINSTANCE instance, void *param)
{
    HWND h = CreateWindowExW(exStyle, className, windowName, style, x, y,
        width, height, parent, menu, instance, param);

    RECT r;
    GetClientRect(h, &r);
    width = width - (r.right - width);
    height = height - (r.bottom - height);

    SetWindowPos(h, HWND_TOP, x, y, width, height, 0);

    return h;
}

void SolveSpace::DoMessageBox(const char *str, int rows, int cols, bool error)
{
    EnableWindow(GraphicsWnd, false);
    EnableWindow(TextWnd, false);

    // Register the window class for our dialog.
    WNDCLASSEX wc = {};
    wc.cbSize           = sizeof(wc);
    wc.style            = CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW | CS_OWNDC;
    wc.lpfnWndProc      = (WNDPROC)MessageProc;
    wc.hInstance        = Instance;
    wc.hbrBackground    = (HBRUSH)COLOR_BTNSHADOW;
    wc.lpszClassName    = L"MessageWnd";
    wc.lpszMenuName     = NULL;
    wc.hCursor          = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon            = (HICON)LoadImage(Instance, MAKEINTRESOURCE(4000),
                            IMAGE_ICON, 32, 32, 0);
    wc.hIconSm          = (HICON)LoadImage(Instance, MAKEINTRESOURCE(4000),
                            IMAGE_ICON, 16, 16, 0);
    RegisterClassEx(&wc);

    // Create the window.
    MessageString = str;
    RECT r;
    GetWindowRect(GraphicsWnd, &r);
    const char *title = error ? "SolveSpace - Error" : "SolveSpace - Message";
    int width  = cols*SS.TW.CHAR_WIDTH + 20,
        height = rows*SS.TW.LINE_HEIGHT + 60;
    MessageWnd = CreateWindowClient(0, L"MessageWnd", Widen(title).c_str(),
        WS_OVERLAPPED | WS_SYSMENU,
        r.left + 100, r.top + 100, width, height, NULL, NULL, Instance, NULL);

    OkButton = CreateWindowExW(0, WC_BUTTON, L"OK",
        WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | WS_VISIBLE | BS_DEFPUSHBUTTON,
        (width - 70)/2, rows*SS.TW.LINE_HEIGHT + 20,
        70, 25, MessageWnd, NULL, Instance, NULL);
    SendMessage(OkButton, WM_SETFONT, (WPARAM)FixedFont, true);

    ShowWindow(MessageWnd, true);
    SetFocus(OkButton);

    MSG msg;
    DWORD ret;
    MessageDone = false;
    while((ret = GetMessage(&msg, NULL, 0, 0)) != 0 && !MessageDone) {
        if((msg.message == WM_KEYDOWN &&
               (msg.wParam == VK_RETURN ||
                msg.wParam == VK_ESCAPE)) ||
            (msg.message == WM_KEYUP &&
               (msg.wParam == VK_SPACE)))
        {
            MessageDone = true;
            break;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    MessageString = NULL;
    EnableWindow(TextWnd, true);
    EnableWindow(GraphicsWnd, true);
    SetForegroundWindow(GraphicsWnd);
    DestroyWindow(MessageWnd);
}

void SolveSpace::AddContextMenuItem(const char *label, ContextCommand cmd)
{
    if(!ContextMenu) ContextMenu = CreatePopupMenu();

    if(cmd == ContextCommand::SUBMENU) {
        AppendMenuW(ContextMenu, MF_STRING | MF_POPUP,
            (UINT_PTR)ContextSubmenu, Widen(label).c_str());
        ContextSubmenu = NULL;
    } else {
        HMENU m = ContextSubmenu ? ContextSubmenu : ContextMenu;
        if(cmd == ContextCommand::SEPARATOR) {
            AppendMenuW(m, MF_SEPARATOR, 0, L"");
        } else {
            AppendMenuW(m, MF_STRING, (uint32_t)cmd, Widen(label).c_str());
        }
    }
}

void SolveSpace::CreateContextSubmenu()
{
    ContextSubmenu = CreatePopupMenu();
}

ContextCommand SolveSpace::ShowContextMenu()
{
    POINT p;
    GetCursorPos(&p);
    int r = TrackPopupMenu(ContextMenu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_TOPALIGN,
        p.x, p.y, 0, GraphicsWnd, NULL);

    DestroyMenu(ContextMenu);
    ContextMenu = NULL;
    return (ContextCommand)r;
}

void CALLBACK TimerCallback(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    // The timer is periodic, so needs to be killed explicitly.
    KillTimer(GraphicsWnd, 1);
    SS.GW.TimerCallback();
    SS.TW.TimerCallback();
}
void SolveSpace::SetTimerFor(int milliseconds)
{
    SetTimer(GraphicsWnd, 1, milliseconds, TimerCallback);
}

void SolveSpace::ScheduleLater()
{
}

static void CALLBACK AutosaveCallback(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    KillTimer(GraphicsWnd, 1);
    SS.Autosave();
}

void SolveSpace::SetAutosaveTimerFor(int minutes)
{
    SetTimer(GraphicsWnd, 2, minutes * 60 * 1000, AutosaveCallback);
}

static void GetWindowSize(HWND hwnd, int *w, int *h)
{
    RECT r;
    GetClientRect(hwnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}
void SolveSpace::GetGraphicsWindowSize(int *w, int *h)
{
    GetWindowSize(GraphicsWnd, w, h);
}
void SolveSpace::GetTextWindowSize(int *w, int *h)
{
    GetWindowSize(TextWnd, w, h);
}

void SolveSpace::OpenWebsite(const char *url) {
    ShellExecuteW(GraphicsWnd, L"open", Widen(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void SolveSpace::ExitNow() {
    PostQuitMessage(0);
}

//-----------------------------------------------------------------------------
// Helpers so that we can read/write registry keys from the platform-
// independent code.
//-----------------------------------------------------------------------------
inline int CLAMP(int v, int a, int b) {
    // Clamp it to the range [a, b]
    if(v <= a) return a;
    if(v >= b) return b;
    return v;
}

static HKEY GetRegistryKey()
{
    HKEY Software;
    if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0,
                     KEY_ALL_ACCESS, &Software) != ERROR_SUCCESS)
        return NULL;

    HKEY SolveSpace;
    if(RegCreateKeyExW(Software, L"SolveSpace", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &SolveSpace, NULL) != ERROR_SUCCESS)
        return NULL;

    RegCloseKey(Software);

    return SolveSpace;
}

void SolveSpace::CnfFreezeInt(uint32_t val, const std::string &name)
{
    HKEY SolveSpace = GetRegistryKey();
    RegSetValueExW(SolveSpace, &Widen(name)[0], 0,
                   REG_DWORD, (const BYTE*) &val, sizeof(DWORD));
    RegCloseKey(SolveSpace);
}
void SolveSpace::CnfFreezeFloat(float val, const std::string &name)
{
    static_assert(sizeof(float) == sizeof(DWORD),
                  "sizes of float and DWORD must match");
    HKEY SolveSpace = GetRegistryKey();
    RegSetValueExW(SolveSpace, &Widen(name)[0], 0,
                   REG_DWORD, (const BYTE*) &val, sizeof(DWORD));
    RegCloseKey(SolveSpace);
}
void SolveSpace::CnfFreezeString(const std::string &str, const std::string &name)
{
    HKEY SolveSpace = GetRegistryKey();
    std::wstring strW = Widen(str);
    RegSetValueExW(SolveSpace, &Widen(name)[0], 0,
                   REG_SZ, (const BYTE*) &strW[0], (strW.length() + 1) * 2);
    RegCloseKey(SolveSpace);
}
static void FreezeWindowPos(HWND hwnd, const std::string &name)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    CnfFreezeInt(r.left,   name + "_left");
    CnfFreezeInt(r.right,  name + "_right");
    CnfFreezeInt(r.top,    name + "_top");
    CnfFreezeInt(r.bottom, name + "_bottom");

    CnfFreezeInt(IsZoomed(hwnd), name + "_maximized");
}

uint32_t SolveSpace::CnfThawInt(uint32_t val, const std::string &name)
{
    HKEY SolveSpace = GetRegistryKey();
    DWORD type, newval, len = sizeof(DWORD);
    LONG result = RegQueryValueEx(SolveSpace, &Widen(name)[0], NULL,
                                  &type, (BYTE*) &newval, &len);
    RegCloseKey(SolveSpace);

    if(result == ERROR_SUCCESS && type == REG_DWORD)
        return newval;
    else
        return val;
}
float SolveSpace::CnfThawFloat(float val, const std::string &name)
{
    HKEY SolveSpace = GetRegistryKey();
    DWORD type, len = sizeof(DWORD);
    float newval;
    LONG result = RegQueryValueExW(SolveSpace, &Widen(name)[0], NULL,
                                   &type, (BYTE*) &newval, &len);
    RegCloseKey(SolveSpace);

    if(result == ERROR_SUCCESS && type == REG_DWORD)
        return newval;
    else
        return val;
}
std::string SolveSpace::CnfThawString(const std::string &val, const std::string &name)
{
    HKEY SolveSpace = GetRegistryKey();
    DWORD type, len;
    if(RegQueryValueExW(SolveSpace, &Widen(name)[0], NULL,
                        &type, NULL, &len) != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(SolveSpace);
        return val;
    }

    std::wstring newval;
    newval.resize(len / 2 - 1);
    if(RegQueryValueExW(SolveSpace, &Widen(name)[0], NULL,
                        NULL, (BYTE*) &newval[0], &len) != ERROR_SUCCESS) {
        RegCloseKey(SolveSpace);
        return val;
    }

    RegCloseKey(SolveSpace);
    return Narrow(newval);
}
static void ThawWindowPos(HWND hwnd, const std::string &name)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    r.left   = CnfThawInt(r.left,   name + "_left");
    r.right  = CnfThawInt(r.right,  name + "_right");
    r.top    = CnfThawInt(r.top,    name + "_top");
    r.bottom = CnfThawInt(r.bottom, name + "_bottom");

    HMONITOR hMonitor = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMonitor, &mi);

    // If it somehow ended up off-screen, then put it back.
    RECT dr = mi.rcMonitor;
    r.left   = CLAMP(r.left,   dr.left, dr.right);
    r.right  = CLAMP(r.right,  dr.left, dr.right);
    r.top    = CLAMP(r.top,    dr.top,  dr.bottom);
    r.bottom = CLAMP(r.bottom, dr.top,  dr.bottom);
    MoveWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);

    if(CnfThawInt(FALSE, name + "_maximized"))
        ShowWindow(hwnd, SW_MAXIMIZE);
}

void SolveSpace::SetCurrentFilename(const std::string &filename) {
    if(!filename.empty()) {
        SetWindowTextW(GraphicsWnd, Widen("SolveSpace - " + filename).c_str());
    } else {
        SetWindowTextW(GraphicsWnd, L"SolveSpace - (not yet saved)");
    }
}

void SolveSpace::SetMousePointerToHand(bool yes) {
    SetCursor(LoadCursor(NULL, yes ? IDC_HAND : IDC_ARROW));
}

static void PaintTextWnd(HDC hdc)
{
    wglMakeCurrent(GetDC(TextWnd), TextGl);

    SS.TW.Paint();
    SwapBuffers(GetDC(TextWnd));

    // Leave the graphics window context active, except when we're painting
    // this text window.
    wglMakeCurrent(GetDC(GraphicsWnd), GraphicsGl);
}

void SolveSpace::MoveTextScrollbarTo(int pos, int maxPos, int page)
{
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_DISABLENOSCROLL | SIF_ALL;
    si.nMin = 0;
    si.nMax = maxPos;
    si.nPos = pos;
    si.nPage = page;
    SetScrollInfo(TextWndScrollBar, SB_CTL, &si, true);
}

void HandleTextWindowScrollBar(WPARAM wParam, LPARAM lParam)
{
    int maxPos, minPos, pos;
    GetScrollRange(TextWndScrollBar, SB_CTL, &minPos, &maxPos);
    pos = GetScrollPos(TextWndScrollBar, SB_CTL);

    switch(LOWORD(wParam)) {
        case SB_LINEUP:         pos--; break;
        case SB_PAGEUP:         pos -= 4; break;

        case SB_LINEDOWN:       pos++; break;
        case SB_PAGEDOWN:       pos += 4; break;

        case SB_TOP:            pos = 0; break;

        case SB_BOTTOM:         pos = maxPos; break;

        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:  pos = HIWORD(wParam); break;
    }

    SS.TW.ScrollbarEvent(pos);
}

static void MouseWheel(int thisDelta) {
    static int DeltaAccum;
    int delta = 0;
    // Handle mouse deltas of less than 120 (like from an un-detented mouse
    // wheel) correctly, even though no one ever uses those.
    DeltaAccum += thisDelta;
    while(DeltaAccum >= 120) {
        DeltaAccum -= 120;
        delta += 120;
    }
    while(DeltaAccum <= -120) {
        DeltaAccum += 120;
        delta -= 120;
    }
    if(delta == 0) return;

    POINT pt;
    GetCursorPos(&pt);
    HWND hw = WindowFromPoint(pt);

    // Make the mousewheel work according to which window the mouse is
    // over, not according to which window is active.
    bool inTextWindow;
    if(hw == TextWnd) {
        inTextWindow = true;
    } else if(hw == GraphicsWnd) {
        inTextWindow = false;
    } else if(GetForegroundWindow() == TextWnd) {
        inTextWindow = true;
    } else {
        inTextWindow = false;
    }

    if(inTextWindow) {
        int i;
        for(i = 0; i < abs(delta/40); i++) {
            HandleTextWindowScrollBar(delta > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        }
    } else {
        SS.GW.MouseScroll(LastMousePos.x, LastMousePos.y, delta);
    }
}

LRESULT CALLBACK TextWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_ERASEBKGND:
            break;

        case WM_CLOSE:
        case WM_DESTROY:
            SolveSpaceUI::MenuFile(Command::EXIT);
            break;

        case WM_PAINT: {
            // Actually paint the text window, with gl.
            PaintTextWnd(GetDC(TextWnd));
            // And then just make Windows happy.
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            break;
        }

        case WM_SIZING: {
            RECT *r = (RECT *)lParam;
            int hc = (r->bottom - r->top) - ClientIsSmallerBy;
            int extra = hc % (SS.TW.LINE_HEIGHT/2);
            switch(wParam) {
                case WMSZ_BOTTOM:
                case WMSZ_BOTTOMLEFT:
                case WMSZ_BOTTOMRIGHT:
                    r->bottom -= extra;
                    break;

                case WMSZ_TOP:
                case WMSZ_TOPLEFT:
                case WMSZ_TOPRIGHT:
                    r->top += extra;
                    break;
            }
            int tooNarrow = (SS.TW.MIN_COLS*SS.TW.CHAR_WIDTH) -
                                                (r->right - r->left);
            if(tooNarrow >= 0) {
                switch(wParam) {
                    case WMSZ_RIGHT:
                    case WMSZ_BOTTOMRIGHT:
                    case WMSZ_TOPRIGHT:
                        r->right += tooNarrow;
                        break;

                    case WMSZ_LEFT:
                    case WMSZ_BOTTOMLEFT:
                    case WMSZ_TOPLEFT:
                        r->left -= tooNarrow;
                        break;
                }
            }
            break;
        }

        case WM_MOUSELEAVE:
            SS.TW.MouseLeave();
            break;

        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE: {
            // We need this in order to get the WM_MOUSELEAVE
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = TextWnd;
            TrackMouseEvent(&tme);

            // And process the actual message
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            SS.TW.MouseEvent(msg == WM_LBUTTONDOWN, wParam & MK_LBUTTON, x, y);
            break;
        }

        case WM_SIZE: {
            RECT r;
            GetWindowRect(TextWndScrollBar, &r);
            int sw = r.right - r.left;
            GetClientRect(hwnd, &r);
            MoveWindow(TextWndScrollBar, r.right - sw, r.top, sw,
                (r.bottom - r.top), true);
            // If the window is growing, then the scrollbar position may
            // be moving, so it's as if we're dragging the scrollbar.
            HandleTextWindowScrollBar((WPARAM)-1, -1);
            InvalidateRect(TextWnd, NULL, false);
            break;
        }

        case WM_MOUSEWHEEL:
            MouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            break;

        case WM_VSCROLL:
            HandleTextWindowScrollBar(wParam, lParam);
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 1;
}

static std::string EditControlText(HWND hwnd)
{
    std::wstring result;
    result.resize(GetWindowTextLength(hwnd));
    GetWindowTextW(hwnd, &result[0], result.length() + 1);
    return Narrow(result);
}

static bool ProcessKeyDown(WPARAM wParam)
{
    if(GraphicsEditControlIsVisible() && wParam != VK_ESCAPE) {
        if(wParam == VK_RETURN) {
            SS.GW.EditControlDone(EditControlText(GraphicsEditControl).c_str());
            return true;
        } else {
            return false;
        }
    }
    if(TextEditControlIsVisible() && wParam != VK_ESCAPE) {
        if(wParam == VK_RETURN) {
            SS.TW.EditControlDone(EditControlText(TextEditControl).c_str());
        } else {
            return false;
        }
    }

    int c;
    switch(wParam) {
        case VK_OEM_PLUS:       c = '+';            break;
        case VK_OEM_MINUS:      c = '-';            break;
        case VK_ESCAPE:         c = 27;             break;
        case VK_OEM_1:          c = ';';            break;
        case VK_OEM_3:          c = '`';            break;
        case VK_OEM_4:          c = '[';            break;
        case VK_OEM_6:          c = ']';            break;
        case VK_OEM_5:          c = '\\';           break;
        case VK_OEM_PERIOD:     c = '.';            break;
        case VK_SPACE:          c = ' ';            break;
        case VK_DELETE:         c = 127;            break;
        case VK_TAB:            c = '\t';           break;

        case VK_BROWSER_BACK:
        case VK_BACK:           c = '\b';           break;

        case VK_F1:
        case VK_F2:
        case VK_F3:
        case VK_F4:
        case VK_F5:
        case VK_F6:
        case VK_F7:
        case VK_F8:
        case VK_F9:
        case VK_F10:
        case VK_F11:
        case VK_F12:            c = ((int)wParam - VK_F1) + 0xf1; break;

        // These overlap with some character codes that I'm using, so
        // don't let them trigger by accident.
        case VK_F16:
        case VK_INSERT:
        case VK_EXECUTE:
        case VK_APPS:
        case VK_LWIN:
        case VK_RWIN:           return false;

        default:
            c = (int)wParam;
            break;
    }
    if(GetAsyncKeyState(VK_SHIFT)   & 0x8000) c |= GraphicsWindow::SHIFT_MASK;
    if(GetAsyncKeyState(VK_CONTROL) & 0x8000) c |= GraphicsWindow::CTRL_MASK;

    switch(c) {
        case GraphicsWindow::SHIFT_MASK | '.': c = '>'; break;
    }

    for(int i = 0; SS.GW.menu[i].level >= 0; i++) {
        if(c == SS.GW.menu[i].accel) {
            (SS.GW.menu[i].fn)((Command)SS.GW.menu[i].id);
            break;
        }
    }

    if(SS.GW.KeyDown(c)) return true;

    // No accelerator; process the key as normal.
    return false;
}

void SolveSpace::ToggleMenuBar()
{
    // Implement me
}
bool SolveSpace::MenuBarIsVisible()
{
    // Implement me
    return true;
}

void SolveSpace::ShowTextWindow(bool visible)
{
    ShowWindow(TextWnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

static void CreateGlContext(HWND hwnd, HGLRC *glrc)
{
    HDC hdc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    int pixelFormat;

    pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                        PFD_DOUBLEBUFFER;
    pfd.dwLayerMask = PFD_MAIN_PLANE;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cAccumBits = 0;
    pfd.cStencilBits = 0;

    pixelFormat = ChoosePixelFormat(hdc, &pfd);
    ssassert(pixelFormat != 0, "Expected a valid pixel format to be chosen");

    ssassert(SetPixelFormat(hdc, pixelFormat, &pfd), "Cannot set pixel format");

    *glrc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, *glrc);
}

void SolveSpace::PaintGraphics()
{
    SS.GW.Paint();
    SwapBuffers(GetDC(GraphicsWnd));
}
void SolveSpace::InvalidateGraphics()
{
    InvalidateRect(GraphicsWnd, NULL, false);
}

void SolveSpace::ToggleFullScreen()
{
    // Implement me
}
bool SolveSpace::FullScreenIsActive()
{
    // Implement me
    return false;
}

int64_t SolveSpace::GetMilliseconds()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    LONGLONG d = t.QuadPart/(f.QuadPart/1000);
    return (int64_t)d;
}

void SolveSpace::InvalidateText()
{
    InvalidateRect(TextWnd, NULL, false);
}

static void ShowEditControl(HWND h, int x, int y, int fontHeight, int minWidthChars,
                            bool isMonospace, const std::wstring &s) {
    static HFONT hf;
    if(hf) DeleteObject(hf);
    hf = CreateFontW(-fontHeight, 0, 0, 0,
        FW_REGULAR, false, false, false, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FF_DONTCARE, isMonospace ? L"Lucida Console" : L"Arial");
    if(hf) SendMessage(h, WM_SETFONT, (WPARAM)hf, false);
    else   SendMessage(h, WM_SETFONT, (WPARAM)(HFONT)GetStockObject(SYSTEM_FONT), false);
    SendMessage(h, EM_SETMARGINS, EC_LEFTMARGIN|EC_RIGHTMARGIN, 0);

    HDC hdc = GetDC(h);
    TEXTMETRICW tm;
    SIZE ts;
    SelectObject(hdc, hf);
    GetTextMetrics(hdc, &tm);
    GetTextExtentPoint32W(hdc, s.c_str(), s.length(), &ts);
    ReleaseDC(h, hdc);

    RECT rc;
    rc.left   = x;
    rc.top    = y - tm.tmAscent;
    // Add one extra char width to avoid scrolling.
    rc.right  = x + std::max(tm.tmAveCharWidth * minWidthChars,
                             ts.cx + tm.tmAveCharWidth);
    rc.bottom = y + tm.tmDescent;

    AdjustWindowRectEx(&rc, 0, false, WS_EX_CLIENTEDGE);
    MoveWindow(h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, true);
    ShowWindow(h, SW_SHOW);
    if(!s.empty()) {
        SendMessage(h, WM_SETTEXT, 0, (LPARAM)s.c_str());
        SendMessage(h, EM_SETSEL, 0, s.length());
        SetFocus(h);
    }
}
void SolveSpace::ShowTextEditControl(int x, int y, const std::string &str)
{
    if(GraphicsEditControlIsVisible()) return;

    ShowEditControl(TextEditControl, x, y, TextWindow::CHAR_HEIGHT, 30,
                    /*isMonospace=*/true, Widen(str));
}
void SolveSpace::HideTextEditControl()
{
    ShowWindow(TextEditControl, SW_HIDE);
}
bool SolveSpace::TextEditControlIsVisible()
{
    return IsWindowVisible(TextEditControl) ? true : false;
}
void SolveSpace::ShowGraphicsEditControl(int x, int y, int fontHeight, int minWidthChars,
                                         const std::string &str)
{
    if(GraphicsEditControlIsVisible()) return;

    RECT r;
    GetClientRect(GraphicsWnd, &r);
    x = x + (r.right - r.left)/2;
    y = (r.bottom - r.top)/2 - y;

    ShowEditControl(GraphicsEditControl, x, y, fontHeight, minWidthChars,
                    /*isMonospace=*/false, Widen(str));
}
void SolveSpace::HideGraphicsEditControl()
{
    ShowWindow(GraphicsEditControl, SW_HIDE);
}
bool SolveSpace::GraphicsEditControlIsVisible()
{
    return IsWindowVisible(GraphicsEditControl) ? true : false;
}

LRESULT CALLBACK GraphicsWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                            LPARAM lParam)
{
    switch (msg) {
        case WM_ERASEBKGND:
            break;

        case WM_SIZE:
            InvalidateRect(GraphicsWnd, NULL, false);
            break;

        case WM_PAINT: {
            // Actually paint the window, with gl.
            PaintGraphics();
            // And make Windows happy.
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            break;
        }

        case WM_MOUSELEAVE:
            SS.GW.MouseLeave();
            break;

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);

            // We need this in order to get the WM_MOUSELEAVE
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = GraphicsWnd;
            TrackMouseEvent(&tme);

            // Convert to xy (vs. ij) style coordinates, with (0, 0) at center
            RECT r;
            GetClientRect(GraphicsWnd, &r);
            x = x - (r.right - r.left)/2;
            y = (r.bottom - r.top)/2 - y;

            LastMousePos.x = x;
            LastMousePos.y = y;

            if(msg == WM_LBUTTONDOWN) {
                SS.GW.MouseLeftDown(x, y);
            } else if(msg == WM_LBUTTONUP) {
                SS.GW.MouseLeftUp(x, y);
            } else if(msg == WM_LBUTTONDBLCLK) {
                SS.GW.MouseLeftDoubleClick(x, y);
            } else if(msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN) {
                SS.GW.MouseMiddleOrRightDown(x, y);
            } else if(msg == WM_RBUTTONUP) {
                SS.GW.MouseRightUp(x, y);
            } else if(msg == WM_MOUSEMOVE) {
                SS.GW.MouseMoved(x, y,
                    !!(wParam & MK_LBUTTON),
                    !!(wParam & MK_MBUTTON),
                    !!(wParam & MK_RBUTTON),
                    !!(wParam & MK_SHIFT),
                    !!(wParam & MK_CONTROL));
            }
            break;
        }
        case WM_MOUSEWHEEL:
            MouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            break;

        case WM_COMMAND: {
            if(HIWORD(wParam) == 0) {
                Command id = (Command)LOWORD(wParam);
                if(((uint32_t)id >= (uint32_t)Command::RECENT_OPEN &&
                    (uint32_t)id < ((uint32_t)Command::RECENT_OPEN + MAX_RECENT))) {
                    SolveSpaceUI::MenuFile(id);
                    break;
                }
                if(((uint32_t)id >= (uint32_t)Command::RECENT_LINK &&
                    (uint32_t)id < ((uint32_t)Command::RECENT_LINK + MAX_RECENT))) {
                    Group::MenuGroup(id);
                    break;
                }
                int i;
                for(i = 0; SS.GW.menu[i].level >= 0; i++) {
                    if(id == SS.GW.menu[i].id) {
                        (SS.GW.menu[i].fn)((Command)id);
                        break;
                    }
                }
                ssassert(SS.GW.menu[i].level >= 0, "Cannot find command in the menu");
            }
            break;
        }

        case WM_CLOSE:
        case WM_DESTROY:
            SolveSpaceUI::MenuFile(Command::EXIT);
            return 1;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 1;
}

//-----------------------------------------------------------------------------
// Common dialog routines, to open or save a file.
//-----------------------------------------------------------------------------
static std::string ConvertFilters(const FileFilter ssFilters[]) {
    std::string filter;
    for(const FileFilter *ssFilter = ssFilters; ssFilter->name; ssFilter++) {
        std::string desc, patterns;
        for(const char *const *ssPattern = ssFilter->patterns; *ssPattern; ssPattern++) {
            std::string pattern = "*." + std::string(*ssPattern);
            if(desc == "")
                desc = pattern;
            else
                desc += ", " + pattern;
            if(patterns == "")
                patterns = pattern;
            else
                patterns += ";" + pattern;
        }
        filter += std::string(ssFilter->name) + " (" + desc + ")" + '\0';
        filter += patterns + '\0';
    }
    filter += '\0';
    return filter;
}

static bool OpenSaveFile(bool isOpen, std::string *filename, const std::string &defExtension,
                         const FileFilter filters[]) {
    // UNC paths may be as long as 32767 characters.
    // Unfortunately, the Get*FileName API does not provide any way to use it
    // except with a preallocated buffer of fixed size, so we use something
    // reasonably large.
    const int len = 32768;
    wchar_t filenameC[len] = {};
    wcsncpy(filenameC, Widen(*filename).c_str(), len - 1);

    std::wstring selPatternW = Widen(ConvertFilters(filters));
    std::wstring defExtensionW = Widen(defExtension);

    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hInstance = Instance;
    ofn.hwndOwner = GraphicsWnd;
    ofn.lpstrFilter = selPatternW.c_str();
    ofn.lpstrDefExt = defExtensionW.c_str();
    ofn.lpstrFile = filenameC;
    ofn.nMaxFile = len;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    EnableWindow(GraphicsWnd, false);
    EnableWindow(TextWnd, false);

    BOOL r;
    if(isOpen) {
        r = GetOpenFileNameW(&ofn);
    } else {
        r = GetSaveFileNameW(&ofn);
    }

    EnableWindow(TextWnd, true);
    EnableWindow(GraphicsWnd, true);
    SetForegroundWindow(GraphicsWnd);

    if(r) *filename = Narrow(filenameC);
    return r ? true : false;
}

bool SolveSpace::GetOpenFile(std::string *filename, const std::string &defExtension,
                             const FileFilter filters[])
{
    return OpenSaveFile(/*isOpen=*/true, filename, defExtension, filters);
}

bool SolveSpace::GetSaveFile(std::string *filename, const std::string &defExtension,
                             const FileFilter filters[])
{
    return OpenSaveFile(/*isOpen=*/false, filename, defExtension, filters);
}

DialogChoice SolveSpace::SaveFileYesNoCancel()
{
    EnableWindow(GraphicsWnd, false);
    EnableWindow(TextWnd, false);

    int r = MessageBoxW(GraphicsWnd,
        L"The file has changed since it was last saved.\n\n"
        L"Do you want to save the changes?", L"SolveSpace",
        MB_YESNOCANCEL | MB_ICONWARNING);

    EnableWindow(TextWnd, true);
    EnableWindow(GraphicsWnd, true);
    SetForegroundWindow(GraphicsWnd);

    switch(r) {
        case IDYES:
        return DIALOG_YES;
        case IDNO:
        return DIALOG_NO;
        case IDCANCEL:
        default:
        return DIALOG_CANCEL;
    }
}

DialogChoice SolveSpace::LoadAutosaveYesNo()
{
    EnableWindow(GraphicsWnd, false);
    EnableWindow(TextWnd, false);

    int r = MessageBoxW(GraphicsWnd,
        L"An autosave file is availible for this project.\n\n"
        L"Do you want to load the autosave file instead?", L"SolveSpace",
        MB_YESNO | MB_ICONWARNING);

    EnableWindow(TextWnd, true);
    EnableWindow(GraphicsWnd, true);
    SetForegroundWindow(GraphicsWnd);

    switch (r) {
        case IDYES:
        return DIALOG_YES;
        case IDNO:
        default:
        return DIALOG_NO;
    }
}

DialogChoice SolveSpace::LocateImportedFileYesNoCancel(const std::string &filename,
                                                       bool canCancel) {
    EnableWindow(GraphicsWnd, false);
    EnableWindow(TextWnd, false);

    std::string message =
        "The linked file " + filename + " is not present.\n\n"
        "Do you want to locate it manually?\n\n"
        "If you select \"No\", any geometry that depends on "
        "the missing file will be removed.";

    int r = MessageBoxW(GraphicsWnd, Widen(message).c_str(), L"SolveSpace",
        (canCancel ? MB_YESNOCANCEL : MB_YESNO) | MB_ICONWARNING);

    EnableWindow(TextWnd, true);
    EnableWindow(GraphicsWnd, true);
    SetForegroundWindow(GraphicsWnd);

    switch(r) {
        case IDYES:
        return DIALOG_YES;
        case IDNO:
        return DIALOG_NO;
        case IDCANCEL:
        default:
        return DIALOG_CANCEL;
    }
}

std::vector<std::string> SolveSpace::GetFontFiles() {
    std::vector<std::string> fonts;

    std::wstring fontsDir(MAX_PATH, '\0');
    fontsDir.resize(GetWindowsDirectoryW(&fontsDir[0], fontsDir.length()));
    fontsDir += L"\\fonts\\";

    WIN32_FIND_DATA wfd;
    HANDLE h = FindFirstFileW((fontsDir + L"*").c_str(), &wfd);
    while(h != INVALID_HANDLE_VALUE) {
        fonts.push_back(Narrow(fontsDir) + Narrow(wfd.cFileName));
        if(!FindNextFileW(h, &wfd)) break;
    }

    return fonts;
}

static void MenuByCmd(Command id, bool yes, bool check)
{
    int i;
    int subMenu = -1;

    for(i = 0; SS.GW.menu[i].level >= 0; i++) {
        if(SS.GW.menu[i].level == 0) subMenu++;

        if(SS.GW.menu[i].id == id) {
            ssassert(subMenu >= 0 && subMenu < (int)arraylen(SubMenus),
                     "Submenu out of range");

            if(check) {
                CheckMenuItem(SubMenus[subMenu], (uint32_t)id,
                            yes ? MF_CHECKED : MF_UNCHECKED);
            } else {
                EnableMenuItem(SubMenus[subMenu], (uint32_t)id,
                            yes ? MF_ENABLED : MF_GRAYED);
            }
            return;
        }
    }
    ssassert(false, "Cannot find submenu");
}
void SolveSpace::CheckMenuByCmd(Command cmd, bool checked)
{
    MenuByCmd(cmd, checked, true);
}
void SolveSpace::RadioMenuByCmd(Command cmd, bool selected)
{
    // Windows does not natively support radio-button menu items
    MenuByCmd(cmd, selected, true);
}
void SolveSpace::EnableMenuByCmd(Command cmd, bool enabled)
{
    MenuByCmd(cmd, enabled, false);
}
static void DoRecent(HMENU m, Command base)
{
    while(DeleteMenu(m, 0, MF_BYPOSITION))
        ;
    int c = 0;
    for(size_t i = 0; i < MAX_RECENT; i++) {
        if(!RecentFile[i].empty()) {
            AppendMenuW(m, MF_STRING, (uint32_t)base + i, Widen(RecentFile[i]).c_str());
            c++;
        }
    }
    if(c == 0) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(no recent files)");
}
void SolveSpace::RefreshRecentMenus()
{
    DoRecent(RecentOpenMenu,   Command::RECENT_OPEN);
    DoRecent(RecentImportMenu, Command::RECENT_LINK);
}

HMENU CreateGraphicsWindowMenus()
{
    HMENU top = CreateMenu();
    HMENU m = 0;

    int i;
    int subMenu = 0;

    for(i = 0; SS.GW.menu[i].level >= 0; i++) {
        std::string label;
        if(SS.GW.menu[i].label) {
            std::string accel = MakeAcceleratorLabel(SS.GW.menu[i].accel);
            const char *sep = accel.empty() ? "" : "\t";
            label = ssprintf("%s%s%s", SS.GW.menu[i].label, sep, accel.c_str());
        }

        if(SS.GW.menu[i].level == 0) {
            m = CreateMenu();
            AppendMenuW(top, MF_STRING | MF_POPUP, (UINT_PTR)m, Widen(label).c_str());
            ssassert(subMenu < (int)arraylen(SubMenus), "Too many submenus");
            SubMenus[subMenu] = m;
            subMenu++;
        } else if(SS.GW.menu[i].level == 1) {
            if(SS.GW.menu[i].id == Command::OPEN_RECENT) {
                RecentOpenMenu = CreateMenu();
                AppendMenuW(m, MF_STRING | MF_POPUP,
                    (UINT_PTR)RecentOpenMenu, Widen(label).c_str());
            } else if(SS.GW.menu[i].id == Command::GROUP_RECENT) {
                RecentImportMenu = CreateMenu();
                AppendMenuW(m, MF_STRING | MF_POPUP,
                    (UINT_PTR)RecentImportMenu, Widen(label).c_str());
            } else if(SS.GW.menu[i].label) {
                AppendMenuW(m, MF_STRING, (uint32_t)SS.GW.menu[i].id, Widen(label).c_str());
            } else {
                AppendMenuW(m, MF_SEPARATOR, (uint32_t)SS.GW.menu[i].id, L"");
            }
        } else ssassert(false, "Submenus nested too deeply");
    }
    RefreshRecentMenus();

    return top;
}

static void CreateMainWindows()
{
    WNDCLASSEX wc = {};

    wc.cbSize = sizeof(wc);

    // The graphics window, where the sketch is drawn and shown.
    wc.style            = CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW | CS_OWNDC |
                          CS_DBLCLKS;
    wc.lpfnWndProc      = (WNDPROC)GraphicsWndProc;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName    = L"GraphicsWnd";
    wc.lpszMenuName     = NULL;
    wc.hCursor          = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon            = (HICON)LoadImage(Instance, MAKEINTRESOURCE(4000),
                            IMAGE_ICON, 32, 32, 0);
    wc.hIconSm          = (HICON)LoadImage(Instance, MAKEINTRESOURCE(4000),
                            IMAGE_ICON, 16, 16, 0);
    ssassert(RegisterClassEx(&wc), "Cannot register window class");

    HMENU top = CreateGraphicsWindowMenus();
    GraphicsWnd = CreateWindowExW(0, L"GraphicsWnd",
        L"SolveSpace (not yet saved)",
        WS_OVERLAPPED | WS_THICKFRAME | WS_CLIPCHILDREN | WS_MAXIMIZEBOX |
        WS_MINIMIZEBOX | WS_SYSMENU | WS_SIZEBOX | WS_CLIPSIBLINGS,
        50, 50, 900, 600, NULL, top, Instance, NULL);
    ssassert(GraphicsWnd != NULL, "Cannot create window");

    GraphicsEditControl = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDIT, L"",
        WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP | WS_CLIPSIBLINGS,
        50, 50, 100, 21, GraphicsWnd, NULL, Instance, NULL);

    // The text window, with a comand line and some textual information
    // about the sketch.
    wc.style           &= ~CS_DBLCLKS;
    wc.lpfnWndProc      = (WNDPROC)TextWndProc;
    wc.hbrBackground    = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName    = L"TextWnd";
    wc.hCursor          = NULL;
    ssassert(RegisterClassEx(&wc), "Cannot register window class");

    // We get the desired Alt+Tab behaviour by specifying that the text
    // window is a child of the graphics window.
    TextWnd = CreateWindowExW(0,
        L"TextWnd", L"SolveSpace - Property Browser", WS_THICKFRAME | WS_CLIPCHILDREN,
        650, 500, 420, 300, GraphicsWnd, (HMENU)NULL, Instance, NULL);
    ssassert(TextWnd != NULL, "Cannot create window");

    TextWndScrollBar = CreateWindowExW(0, WC_SCROLLBAR, L"", WS_CHILD |
        SBS_VERT | SBS_LEFTALIGN | WS_VISIBLE | WS_CLIPSIBLINGS,
        200, 100, 100, 100, TextWnd, NULL, Instance, NULL);
    // Force the scrollbar to get resized to the window,
    TextWndProc(TextWnd, WM_SIZE, 0, 0);

    TextEditControl = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDIT, L"",
        WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP | WS_CLIPSIBLINGS,
        50, 50, 100, 21, TextWnd, NULL, Instance, NULL);

    // Now that all our windows exist, set up gl contexts.
    CreateGlContext(TextWnd, &TextGl);
    CreateGlContext(GraphicsWnd, &GraphicsGl);

    RECT r, rc;
    GetWindowRect(TextWnd, &r);
    GetClientRect(TextWnd, &rc);
    ClientIsSmallerBy = (r.bottom - r.top) - (rc.bottom - rc.top);
}

const void *SolveSpace::LoadResource(const std::string &name, size_t *size) {
    HRSRC hres = FindResourceW(NULL, Widen(name).c_str(), RT_RCDATA);
    ssassert(hres != NULL, "Cannot find resource");
    HGLOBAL res = LoadResource(NULL, hres);
    ssassert(res != NULL, "Cannot load resource");

    *size = SizeofResource(NULL, hres);
    return LockResource(res);
}

#ifdef HAVE_SPACEWARE
//-----------------------------------------------------------------------------
// Test if a message comes from the SpaceNavigator device. If yes, dispatch
// it appropriately and return true. Otherwise, do nothing and return false.
//-----------------------------------------------------------------------------
static bool ProcessSpaceNavigatorMsg(MSG *msg) {
    if(SpaceNavigator == SI_NO_HANDLE) return false;

    SiGetEventData sged;
    SiSpwEvent sse;

    SiGetEventWinInit(&sged, msg->message, msg->wParam, msg->lParam);
    int ret = SiGetEvent(SpaceNavigator, 0, &sged, &sse);
    if(ret == SI_NOT_EVENT) return false;
    // So the device is a SpaceNavigator event, or a SpaceNavigator error.

    if(ret == SI_IS_EVENT) {
        if(sse.type == SI_MOTION_EVENT) {
            // The Z axis translation and rotation are both
            // backwards in the default mapping.
            double tx =  sse.u.spwData.mData[SI_TX]*1.0,
                   ty =  sse.u.spwData.mData[SI_TY]*1.0,
                   tz = -sse.u.spwData.mData[SI_TZ]*1.0,
                   rx =  sse.u.spwData.mData[SI_RX]*0.001,
                   ry =  sse.u.spwData.mData[SI_RY]*0.001,
                   rz = -sse.u.spwData.mData[SI_RZ]*0.001;
            SS.GW.SpaceNavigatorMoved(tx, ty, tz, rx, ry, rz,
                !!(GetAsyncKeyState(VK_SHIFT) & 0x8000));
        } else if(sse.type == SI_BUTTON_EVENT) {
            int button;
            button = SiButtonReleased(&sse);
            if(button == SI_APP_FIT_BUTTON) SS.GW.SpaceNavigatorButtonUp();
        }
    }
    return true;
}
#endif // HAVE_SPACEWARE

//-----------------------------------------------------------------------------
// Entry point into the program.
//-----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPSTR lpCmdLine, INT nCmdShow)
{
    Instance = hInstance;

    InitCommonControls();

    // A monospaced font
    FixedFont = CreateFontW(SS.TW.CHAR_HEIGHT, SS.TW.CHAR_WIDTH, 0, 0,
        FW_REGULAR, false,
        false, false, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FF_DONTCARE, L"Lucida Console");
    if(!FixedFont)
        FixedFont = (HFONT)GetStockObject(SYSTEM_FONT);

    // Create the root windows: one for control, with text, and one for
    // the graphics
    CreateMainWindows();

    ThawWindowPos(TextWnd, "TextWnd");
    ThawWindowPos(GraphicsWnd, "GraphicsWnd");

    ShowWindow(TextWnd, SW_SHOWNOACTIVATE);
    ShowWindow(GraphicsWnd, SW_SHOW);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers(GetDC(GraphicsWnd));
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers(GetDC(GraphicsWnd));

    // Create the heaps for all dynamic memory (AllocTemporary, MemAlloc)
    InitHeaps();

    // Pull out the Unicode command line.
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // A filename may have been specified on the command line; if so, then
    // strip any quotation marks, and make it absolute.
    std::wstring filenameRel, filename;
    if(argc >= 2) {
        filenameRel = argv[1];
        if(filenameRel[0] == L'\"' && filenameRel[filenameRel.length() - 1] == L'\"') {
            filenameRel.erase(0, 1);
            filenameRel.erase(filenameRel.length() - 1, 1);
        }

        DWORD len = GetFullPathNameW(&filenameRel[0], 0, NULL, NULL);
        filename.resize(len - 1);
        GetFullPathNameW(&filenameRel[0], len, &filename[0], NULL);
    }

#ifdef HAVE_SPACEWARE
    // Initialize the SpaceBall, if present. Test if the driver is running
    // first, to avoid a long timeout if it's not.
    HWND swdc = FindWindowW(L"SpaceWare Driver Class", NULL);
    if(swdc != NULL) {
        SiOpenData sod;
        SiInitialize();
        SiOpenWinInit(&sod, GraphicsWnd);
        SpaceNavigator =
            SiOpen("GraphicsWnd", SI_ANY_DEVICE, SI_NO_MASK, SI_EVENT, &sod);
        SiSetUiMode(SpaceNavigator, SI_UI_NO_CONTROLS);
    }
#endif

    // Call in to the platform-independent code, and let them do their init
    SS.Init();
    if(!filename.empty())
        SS.OpenFile(Narrow(filename));

    // And now it's the message loop. All calls in to the rest of the code
    // will be from the wndprocs.
    MSG msg;
    DWORD ret;
    while((ret = GetMessage(&msg, NULL, 0, 0)) != 0) {
#ifdef HAVE_SPACEWARE
        // Is it a message from the six degree of freedom input device?
        if(ProcessSpaceNavigatorMsg(&msg)) goto done;
#endif

        // A message from the keyboard, which should be processed as a keyboard
        // accelerator?
        if(msg.message == WM_KEYDOWN) {
            if(ProcessKeyDown(msg.wParam)) goto done;
        }
        if(msg.message == WM_SYSKEYDOWN && msg.hwnd == TextWnd) {
            // If the user presses the Alt key when the text window has focus,
            // then that should probably go to the graphics window instead.
            SetForegroundWindow(GraphicsWnd);
        }

        // None of the above; so just a normal message to process.
        TranslateMessage(&msg);
        DispatchMessage(&msg);
done:
        SS.DoLater();
    }

#ifdef HAVE_SPACEWARE
    if(swdc != NULL) {
        if(SpaceNavigator != SI_NO_HANDLE) SiClose(SpaceNavigator);
        SiTerminate();
    }
#endif

    // Write everything back to the registry
    FreezeWindowPos(TextWnd, "TextWnd");
    FreezeWindowPos(GraphicsWnd, "GraphicsWnd");

    // Free the memory we've used; anything that remains is a leak.
    SK.Clear();
    SS.Clear();

    return 0;
}

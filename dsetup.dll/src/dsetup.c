#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <ole2.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "dsetup.h"

#define DSETUP_C     "d:\\bt\\234\\private\\multimedia\\directx\\setup\\dsetup\\dsetup\\dsetup.c"
#define DSINLINE_C   "d:\\bt\\234\\private\\multimedia\\directx\\setup\\dsetup\\dsetup\\dsinline.c"

#define E_STR_INVALIDARG     ((HRESULT)0x80070057L)
#define E_STR_INSUFFICIENT   ((HRESULT)0x8007007AL)

#define DXSETUP_ERR_BUSY     (-9)   /* returned when setup cannot run */

#define STR_ID_APPKEY_FMT    0x7d0  /* "SOFTWARE\...\Applications\%s"   */
#define STR_ID_GUID          0x7d1
#define STR_ID_FILE          0x7d2
#define STR_ID_COMMANDLINE   0x7d3
#define STR_ID_CURRENTDIR    0x7d4
#define STR_ID_PATH          0x7d5
#define STR_ID_LAUNCHER      0x7d6
#define STR_ID_SETUP_ABORTED 0x1391 /* text shown by the *DeviceDriverSetup/DirectXSetupW fallbacks */
#define STR_ID_CAPTION       0x3e8  /* MessageBox caption */

typedef INT   (WINAPI *PFN_XCALLBACK)(DWORD, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR);
typedef DWORD (WINAPI *PFN_XGETEULA)(LPSTR, DWORD, WORD);
typedef INT   (WINAPI *PFN_XSETUP)(HWND, LPSTR, DWORD);
typedef DWORD (WINAPI *PFN_XSETCALLBACK)(DSETUP_CALLBACK);
typedef UINT  (WINAPI *PFN_XLOADSTRING)(UINT, LPSTR, UINT);
typedef INT   (WINAPI *PFN_XSHOWEULA)(HWND);

typedef BOOL  (WINAPI *PFN_ISNTADMIN)(DWORD, DWORD *);

static HINSTANCE        g_hInst;           /* this DLL                       */
static HMODULE          g_hDSetup32;       /* DSETUP32.DLL                   */
static HWND             g_hWndParent;      /* last hWnd passed to a setup API*/
static DSETUP_CALLBACK  g_pfnUserCB;       /* callback set before dsetup32   */
static DWORD            g_dwSetupFlags;
static HWND             g_hDlg;            /* progress dialog                */
static HANDLE           g_hMutex;          /* "DSETUP DLL Mutex"             */
static LONG             g_lRefCount;

static PFN_XCALLBACK    g_pfnXCallback;    /* DirectXSetupCallback           */
static PFN_XGETEULA     g_pfnXGetEULA;     /* iDirectXSetupGetEULA           */
static PFN_XSETUP       g_pfnXSetup;       /* iDirectXSetup                  */
static PFN_XSETCALLBACK g_pfnXSetCallback; /* DirectXSetupSetCallback        */
static PFN_XLOADSTRING  g_pfnXLoadString;  /* DirectXLoadString              */
static PFN_XSHOWEULA    g_pfnXShowEULA;    /* DirectXSetupShowEULA           */

static char g_szDXLog[0x108];              /* "<windir>\DirectX.log"         */
static char g_szDXErrLog[0x108];           /* "<windir>\DXError.log"         */
static char g_szFileInProcess[0x108];      /* always empty in practice       */

static HRESULT StrVPrintfA(char *dst, size_t cch, const char *fmt, va_list ap)
{
    size_t max;
    int n;
    if (cch == 0 || cch > 0x7fffffff) return E_STR_INVALIDARG;
    max = cch - 1;
    n = _vsnprintf(dst, max, fmt, ap);
    if (n < 0 || (size_t)n > max) {
        dst[max] = 0;
        return E_STR_INSUFFICIENT;
    }
    if ((size_t)n == max) dst[max] = 0;
    return S_OK;
}

static HRESULT StrVPrintfW(wchar_t *dst, size_t cch, const wchar_t *fmt, va_list ap)
{
    size_t max;
    int n;
    if (cch == 0 || cch > 0x7fffffff) return E_STR_INVALIDARG;
    max = cch - 1;
    n = _vsnwprintf(dst, max, fmt, ap);
    if (n < 0 || (size_t)n > max) {
        dst[max] = 0;
        return E_STR_INSUFFICIENT;
    }
    if ((size_t)n == max) dst[max] = 0;
    return S_OK;
}

static HRESULT StrPrintfA(char *dst, size_t cch, const char *fmt, ...)
{
    va_list ap; HRESULT hr;
    va_start(ap, fmt);
    hr = StrVPrintfA(dst, cch, fmt, ap);
    va_end(ap);
    return hr;
}

static HRESULT StrPrintfW(wchar_t *dst, size_t cch, const wchar_t *fmt, ...)
{
    va_list ap; HRESULT hr;
    va_start(ap, fmt);
    hr = StrVPrintfW(dst, cch, fmt, ap);
    va_end(ap);
    return hr;
}

static HRESULT StrCopyA(char *dst, size_t cch, const char *src)
{
    size_t i;
    if (cch == 0 || cch > 0x7fffffff) return E_STR_INVALIDARG;
    for (i = 0; i < cch - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return src[i] ? E_STR_INSUFFICIENT : S_OK;
}

static HRESULT StrCatA(char *dst, size_t cch, const char *src)
{
    size_t len = 0;
    while (len < cch && dst[len]) len++;
    if (len == cch) return E_STR_INVALIDARG;           /* dst not terminated */
    return StrCopyA(dst + len, cch - len, src);
}

/* ------------------------------------------------------------------------- */
/* Logging: DirectX.log / DXError.log in the Windows directory               */
/* ------------------------------------------------------------------------- */

/* 0x4060a9: append one line "<date> <time>: DSETUP: <msg>\n" to DirectX.log */
static void LogToDXLog(const char *msg)
{
    char  line[0x258];
    char  date[16], tm[16];
    FILE *fp;

    if (g_szDXLog[0] == 0) {
        char win[0x104];
        if (GetWindowsDirectoryA(win, 0x104) == 0) {
            OutputDebugStringA("DXSETUP_DPF(): GetWindowsDirectory() failed.\n");
            return;
        }
        if (StrPrintfA(g_szDXLog, 0x104, "%s%s%s", win, "\\", "DirectX.log") < 0) {
            OutputDebugStringA("DXSETUP_DPF(): path name too long.\n");
            return;
        }
    }
    fp = fopen(g_szDXLog, "a+");
    if (!fp) {
        OutputDebugStringA("DXSETUP_DPF(): Unable to open log file.\n");
        return;
    }
    _strdate(date);
    _strtime(tm);
    if (msg)
        StrPrintfA(line, 0x258, "%s %s: %s: %s\n", date, tm, "DSETUP", msg);
    else
        StrPrintfA(line, 0x258, "%s %s: %s: (null)\n", date, tm, "DSETUP");
    fputs(line, fp);
    fclose(fp);
}

/* 0x406207: append raw text to DXError.log */
static void LogToErrLog(const char *text)
{
    FILE *fp;

    if (g_szDXErrLog[0] == 0) {
        char win[0x104];
        if (GetWindowsDirectoryA(win, 0x104) == 0) {
            OutputDebugStringA("DXSETUP_DPF(): GetWindowsDirectory() failed.\n");
            return;
        }
        if (StrPrintfA(g_szDXErrLog, 0x104, "%s%s%s", win, "\\", "DXError.log") < 0) {
            OutputDebugStringA("DXSETUP_DPF(): path name too long.\n");
            return;
        }
    }
    fp = fopen(g_szDXErrLog, "a+");
    if (!fp) {
        OutputDebugStringA("DXSETUP_DPF(): Unable to open log file.\n");
        return;
    }
    fputs(text, fp);
    fclose(fp);
}

/* 0x4061c0: printf-style debug print into DirectX.log */
static void DXSETUP_DPF(const char *fmt, ...)
{
    char buf[0x258];
    va_list ap;
    va_start(ap, fmt);
    StrVPrintfA(buf, 0x258, fmt, ap);
    va_end(ap);
    LogToDXLog(buf);
}

/* 0x4062b4: structured error report (DXError.log + DirectX.log) */
static void DXSError(const char *file, int line, const char *func,
                     const char *api, int err, const char *fmt, ...)
{
    char  buf[0x258];
    char  buf2[0x104];
    char  date[16], tm[16];
    const char *base;
    char *slash;
    va_list ap;

    _strdate(date);
    _strtime(tm);

    LogToErrLog("--------------------\n");
    StrPrintfA(buf, 0x258, "[%s %s]", date, tm);
    LogToErrLog(buf);

    slash = strrchr(file, '\\');
    base = (slash && slash[1] != 0) ? slash + 1 : file;
    StrPrintfA(buf, 0x258, " module: %s(%s), file: %s, line: %d, function: %s\n\n",
               "DSETUP", "Mar 31 2006", base, line, func);
    LogToErrLog(buf);

    if (api) {
        StrPrintfA(buf, 0x258, "    Failed API:\t\t%s\n", api);
        LogToErrLog(buf);

        if (err == 0)
            StrPrintfA(buf, 0x258, "%s(): %s failed.", func, api);
        else if (err >= 0)
            StrPrintfA(buf, 0x258, "%s(): %s failed, error = %d.", func, api, err);
        else
            StrPrintfA(buf, 0x258, "%s(): %s failed, error = 0x%x.", func, api, err);
        LogToDXLog(buf);
    }

    if (err != 0) {
        char *msg = NULL;
        LogToErrLog("    Error:\t\t");
        StrPrintfA(buf, 0x258, err < 0 ? "(0x%x)" : "(%d)", err);
        LogToErrLog(buf);

        if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, (DWORD)err, 0, (LPSTR)&msg, 0, NULL)) {
            LogToErrLog(" - ");
            LogToErrLog(msg);
            LocalFree(msg);
        } else {
            DWORD gle = GetLastError();
            if (gle == ERROR_MR_MID_NOT_FOUND)
                DXSETUP_DPF("DXSError(): FormatMessage() failed, system cannot find message text for error.");
            else
                DXSETUP_DPF("DXSError(): FormatMessage() failed, error = %d.", (int)gle);
        }
        LogToErrLog("\n\n");
    } else if (api) {
        LogToErrLog("\n");
    }

    if (g_szFileInProcess[0]) {
        LogToErrLog("    File in process:\t");
        LogToErrLog(g_szFileInProcess);
        LogToErrLog("\n");
    }

    if (fmt) {
        va_start(ap, fmt);
        StrVPrintfA(buf, 0x258, fmt, ap);
        va_end(ap);
        LogToErrLog("    ");
        LogToErrLog(buf);
        LogToErrLog("\n\n");

        StrPrintfA(buf2, 0x104, "%s(): %s", func, fmt);
        va_start(ap, fmt);
        StrVPrintfA(buf, 0x258, buf2, ap);
        va_end(ap);
        LogToDXLog(buf);
    }
}

#define ERR(line, func, api, err, ...) \
    DXSError(DSETUP_C, (line), (func), (api), (int)(err), __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Path / string utilities from dsinline.c                                   */
/* ------------------------------------------------------------------------- */

/* 0x405ef1: pointer to the file-name part of a path */
static const char *DXFindFileName(const char *p)
{
    const char *e = p + strlen(p);
    while (e != p) {
        if (*e == '\\' || *e == ':') return e + 1;
        e--;
    }
    return p;
}

/* 0x405f2c: pointer to the extension (without the dot) or to the end */
static const char *DXFindExtension(const char *p)
{
    const char *e = p + strlen(p);
    while (e != p) {
        if (*e == '.') return e + 1;
        e--;
    }
    return p + strlen(p);
}

/* 0x405db5: TRUE when strings differ (case-insensitive, en-US) */
static BOOL DXStrNotEqualI(const char *a, const char *b)
{
    return CompareStringA(0x409, NORM_IGNORECASE, a, -1, b, -1) != CSTR_EQUAL;
}

/* 0x405f6e: dst = a + b (StringCchPrintf "%s%s") */
static BOOL DXBuildPath(char *dst, const char *a, const char *b)
{
    HRESULT hr = StrPrintfA(dst, 0x104, "%s%s", a, b);
    if (hr < 0) {
        DXSError(DSINLINE_C, 0x1df, "DXBuildPath", "StringCchPrintf()", (int)hr, NULL);
        return FALSE;
    }
    return TRUE;
}

/* 0x405fc3: "4.09.00.0904" -> { MS, LS } */
static BOOL StringToVersionInfo(const char *s, DWORD out[2])
{
    char  buf[0x104];
    char *t;
    int   a, b, c, d;

    StrCopyA(buf, 0x104, s);
    t = strtok(buf, ". ");   if (!t) goto fail; a = atoi(t);
    t = strtok(NULL, ". ");  if (!t) goto fail; b = atoi(t);
    t = strtok(NULL, ". ");  if (!t) goto fail; c = atoi(t);
    t = strtok(NULL, ". ");  if (!t) goto fail; d = atoi(t);

    out[0] = ((DWORD)a << 16) | ((DWORD)b & 0xffff);
    out[1] = ((DWORD)c << 16) | ((DWORD)d & 0xffff);
    return TRUE;
fail:
    out[0] = 0;
    out[1] = 0;
    return FALSE;
}

/* 0x405de4: IsAdmin (dsinline.c) */
static BOOL IsAdmin(void)
{
    OSVERSIONINFOA vi;
    BOOL    result = FALSE;
    HMODULE h;
    PFN_ISNTADMIN pfn;

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = 0x94;           /* OSVERSIONINFOEXA-sized */
    GetVersionExA(&vi);

    if (vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
        return TRUE;
    if (vi.dwPlatformId == VER_PLATFORM_WIN32_NT &&
        vi.dwMajorVersion == 4 && vi.dwMinorVersion == 0)
        return TRUE;

    h = LoadLibraryA("advpack.dll");
    if (!h) {
        DXSError(DSINLINE_C, 0x7b, "IsAdmin", "LoadLibrary()", (int)GetLastError(),
                 "Unable to load %s.", "advpack.dll");
        return result;
    }
    pfn = (PFN_ISNTADMIN)GetProcAddress(h, "IsNTAdmin");
    if (pfn) {
        DWORD dummy;
        result = pfn(0, &dummy);
    } else {
        DXSError(DSINLINE_C, 0x78, "IsAdmin", "GetProcAddress()", (int)GetLastError(),
                 "Module: %s, Function: %s", "advpack.dll", "IsNTAdmin");
    }
    FreeLibrary(h);
    return result;
}

/* ------------------------------------------------------------------------- */
/* DllMain                                                                   */
/* ------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_DETACH) {
        OutputDebugStringA("DLL_PROCESS_DETACH");
        if (--g_lRefCount != 0)
            return TRUE;

        g_pfnUserCB = NULL;
        if (!CloseHandle(g_hMutex))
            ERR(0x9b, "DllMain", "CloseHandle()", GetLastError(), NULL);

        if (g_hDSetup32) {
            if (!FreeLibrary(g_hDSetup32)) {
                DWORD e = GetLastError();
                if (e)
                    ERR(0xa2, "DllMain", "FreeLibrary()", e, NULL);
            }
        }
        return TRUE;
    }

    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE h;
        OutputDebugStringA("DLL_PROCESS_ATTACH");
        h = CreateMutexA(NULL, FALSE, "DSETUP DLL Mutex");
        g_lRefCount++;
        if (!h) {
            ERR(0x74, "DllMain", "CreateMutex()", GetLastError(), NULL);
            return FALSE;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            /* QUIRK: second instance only closes its handle and does not
             * record hInst / mutex in the globals. */
            if (!CloseHandle(h)) {
                ERR(0x7b, "DllMain", "CloseHandle()", GetLastError(), NULL);
                return FALSE;
            }
            return TRUE;
        }
        g_hInst      = hInst;
        g_hMutex     = h;
        g_pfnUserCB  = NULL;
        g_hDSetup32  = NULL;
        return TRUE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* DirectX version detection                                                 */
/* ------------------------------------------------------------------------- */

/* 0x403945 */
static BOOL GetRegistryDXVersion(DWORD out[2])
{
    HKEY  hKey = NULL;
    char  ver[0x104];
    DWORD cb = 0x104, type;
    LONG  r;
    BOOL  ok = FALSE;

    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "software\\microsoft\\directx", 0, KEY_QUERY_VALUE, &hKey);
    if (r != ERROR_SUCCESS) {
        ERR(0xbf, "GetRegistryDXVersion", "RegOpenKeyEx()", r, NULL);
        return FALSE;
    }
    r = RegQueryValueExA(hKey, "Version", NULL, &type, (LPBYTE)ver, &cb);
    if (r == ERROR_SUCCESS) {
        if (StringToVersionInfo(ver, out))
            ok = TRUE;
        else
            ERR(0xd0, "GetRegistryDXVersion", NULL, 0, "StringToVersionInfo() failed.");
    } else {
        ERR(0xd4, "GetRegistryDXVersion", "RegQueryValueEx()", r, NULL);
    }
    r = RegCloseKey(hKey);
    if (r != ERROR_SUCCESS)
        ERR(0xd7, "GetRegistryDXVersion", "RegCloseKey()", r, NULL);
    return ok;
}

/* 0x403a75 */
static BOOL GetRunningDXVersion(DWORD out[2])
{
    char   cwd[0x104];
    char   sysdir[0x108];
    char   dinput[0x104];
    OSVERSIONINFOA vi;
    const char *F = "GetRunningDXVersion";

    if (!GetCurrentDirectoryA(0x104, cwd)) {
        ERR(0xeb, F, "GetCurrentDirectory()", GetLastError(), NULL);
        return FALSE;
    }
    if (!GetSystemDirectoryA(sysdir, 0x104)) {
        ERR(0xf0, F, "GetSystemDirectory()", GetLastError(), NULL);
        return FALSE;
    }
    if (!SetCurrentDirectoryA(sysdir)) {
        ERR(0xf5, F, "SetCurrentDirectory()", GetLastError(), NULL);
        return FALSE;
    }

    out[0] = 0;
    out[1] = 0;

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = 0x94;
    if (!GetVersionExA(&vi)) {
        ERR(0x103, F, "GetVersionEx()", GetLastError(), NULL);
        goto done_noreset;
    }

    if (vi.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        if (vi.dwMajorVersion >= 4) {
            if (vi.dwMajorVersion == 4) {
                HMODULE h;
                FARPROC p;
                out[0] = 0x40002;
                if (!DXBuildPath(dinput, sysdir, "\\DINPUT.DLL")) {
                    ERR(0x11d, F, NULL, 0, "Unable to create path string, %s%s.", sysdir, "\\DINPUT.DLL");
                } else {
                    h = LoadLibraryA(dinput);
                    if (!h) {
                        ERR(0x128, F, "LoadLibrary()", GetLastError(), NULL);
                    } else {
                        p = GetProcAddress(h, "DirectInputCreateA");
                        if (!p)
                            ERR(0x12d, F, "GetProcAddress()", GetLastError(),
                                "Module: %d, Function: DirectXInputCreateA", (int)(DWORD_PTR)dinput);
                        if (!FreeLibrary(h))
                            ERR(0x130, F, "FreeLibrary()", GetLastError(), NULL);
                        if (p)
                            out[0] = 0x40003;
                    }
                }
            } else {
                out[0] = 0x40006;
                out[1] = 0;
            }
        }
    } else {
        if ((vi.dwBuildNumber & 0xffff) > 0x549) {
            out[0] = 0x40005;
            out[1] = 0x10000;
        } else {
            out[0] = 0;
            out[1] = 0;
        }
    }

done_noreset:
    if (!SetCurrentDirectoryA(cwd)) {
        ERR(0x162, F, "SetCurrentDirectory()", GetLastError(), NULL);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* dsetup32.dll loader (0x404abc)                                            */
/* ------------------------------------------------------------------------- */

static BOOL LoadDSetup32(void)
{
    const char *F = "LoadDSetup32";

    if (!g_hDSetup32) {
        char cwd[0x104];
        char path[0x104];
        char *bs;

        if (!GetCurrentDirectoryA(0x104, cwd)) {
            ERR(0x192, F, "GetCurrentDirectory()", GetLastError(), NULL);
            return FALSE;
        }
        if (!GetModuleFileNameA(g_hInst, path, 0x104)) {
            ERR(0x198, F, "GetModuleFileName()", GetLastError(), NULL);
            return FALSE;
        }
        bs = strrchr(path, '\\');
        if (!bs) {
            ERR(0x1a0, F, NULL, 0, "Unable to find backslash.");
            return FALSE;
        }
        *bs = 0;
        if (!SetCurrentDirectoryA(path)) {
            ERR(0x1a5, F, "SetCurrentDirectory()", GetLastError(), NULL);
            return FALSE;
        }
        if (StrCatA(path, 0x104, "\\DSETUP32.DLL") < 0) {
            /* QUIRK: cwd is not restored on this path */
            ERR(0x1ab, F, NULL, 0, "Unable to create path string, %s%s.", path, "\\DSETUP32.DLL");
            return FALSE;
        }
        g_hDSetup32 = LoadLibraryA(path);
        if (!SetCurrentDirectoryA(cwd)) {
            ERR(0x1b3, F, "SetCurrentDirectory()", GetLastError(), NULL);
            return FALSE;
        }
        if (!g_hDSetup32) {
            ERR(0x1ba, F, "LoadLibrary()", GetLastError(), NULL);
            return FALSE;
        }
    }

    if (!g_pfnXSetup) {
        g_pfnXSetup = (PFN_XSETUP)GetProcAddress(g_hDSetup32, "iDirectXSetup");
        if (!g_pfnXSetup) {
            ERR(0x1c6, F, "GetProcAddress()", GetLastError(),
                "Module: dsetup32.dll, Function: iDirectXSetup");
            return FALSE;
        }
    }
    if (!g_pfnXCallback) {
        g_pfnXCallback = (PFN_XCALLBACK)GetProcAddress(g_hDSetup32, "DirectXSetupCallback");
        if (!g_pfnXCallback) {
            ERR(0x1d0, F, "GetProcAddress()", GetLastError(),
                "Module: dsetup32.dll, Function: DirectXSetupCallback");
            return FALSE;
        }
    }
    if (!g_pfnXSetCallback) {
        g_pfnXSetCallback = (PFN_XSETCALLBACK)GetProcAddress(g_hDSetup32, "DirectXSetupSetCallback");
        if (!g_pfnXSetCallback) {
            ERR(0x1da, F, "GetProcAddress()", GetLastError(),
                "Module: dsetup32.dll, Function: DirectXSetupSetCallback");
            return FALSE;
        }
        /* hand over the callback registered before dsetup32 was loaded */
        g_pfnXSetCallback(g_pfnUserCB);
    }
    if (!g_pfnXLoadString) {
        g_pfnXLoadString = (PFN_XLOADSTRING)GetProcAddress(g_hDSetup32, "DirectXLoadString");
        if (!g_pfnXLoadString) {
            ERR(0x1e6, F, "GetProcAddress()", GetLastError(),
                "Module: dsetup32.dll, Function: DirectXLoadString");
            return FALSE;
        }
    }
    if (!g_pfnXShowEULA) {
        g_pfnXShowEULA = (PFN_XSHOWEULA)GetProcAddress(g_hDSetup32, "DirectXSetupShowEULA");
        if (!g_pfnXShowEULA) {
            ERR(0x1f0, F, "GetProcAddress()", GetLastError(),
                "Module: dsetup32.dll, Function: DirectXSetupShowEULA");
            return FALSE;
        }
    }
    if (!g_pfnXGetEULA) {
        g_pfnXGetEULA = (PFN_XGETEULA)GetProcAddress(g_hDSetup32, "iDirectXSetupGetEULA");
        if (!g_pfnXGetEULA) {
            ERR(0x1fa, F, "GetProcAddress()", GetLastError(),
                "Module: dsetup32.dll,  Function: iDirectXSetupGetEULA");
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Simple exports                                                            */
/* ------------------------------------------------------------------------- */

INT WINAPI DirectXSetupGetVersion(DWORD *pdwVersion, DWORD *pdwRevision)
{
    DWORD v[2] = { 0, 0 };

    if (!GetRegistryDXVersion(v)) {
        if (!GetRunningDXVersion(v))
            ERR(0x174, "DirectXSetupGetVersion", NULL, 0, "GetRunningDXVersion() failed.");
    }
    if (pdwVersion)  *pdwVersion  = v[0];
    if (pdwRevision) *pdwRevision = v[1];
    return TRUE;
}

INT WINAPI DirectXSetupIsJapan(void)
{
    if (GetSystemDefaultLCID() == 0x411) {
        DXSETUP_DPF("DirectXSetupIsJapan == 1");
        return TRUE;
    }
    DXSETUP_DPF("DirectXSetupIsJapan == 0");
    return FALSE;
}

INT WINAPI DirectXSetupIsEng(void)
{
    if (GetSystemDefaultLCID() == 0x409) {
        DXSETUP_DPF("DirectXSetupIsEng == 1");
        return TRUE;
    }
    DXSETUP_DPF("DirectXSetupIsEng == 0");
    return FALSE;
}

/* 0x4049b1: NEC PC-98 keyboard probe */
static BOOL distinction98(void)
{
    if (GetKeyboardType(0) != 7) {
        DXSETUP_DPF("distinction98 failed GetKeyboardType");
        return FALSE;
    }
    if ((GetKeyboardType(1) & 0xd00) == 0xd00)
        return TRUE;
    DXSETUP_DPF("distinction98 failed generally");
    return FALSE;
}

INT WINAPI DirectXSetupIsJapanNec(void)
{
    if (DirectXSetupIsJapan() && distinction98()) {
        DXSETUP_DPF("DirectXSetupIsJapanNec == 1");
        return TRUE;
    }
    DXSETUP_DPF("DirectXSetupIsJapanNec == 0");
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Callback / progress dialog                                                */
/* ------------------------------------------------------------------------- */

/* 0x403dae: drain the message queue */
static void PumpMessages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE)) {
        if (GetMessageA(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

/* 0x403da4 */
static INT_PTR CALLBACK ProgressDlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)h; (void)m; (void)w; (void)l;
    return 0;
}

INT WINAPI DirectXSetupCallback(DWORD Reason, DWORD MsgType, LPSTR szMessage,
                                LPSTR szName, void *pInfo)
{
    PumpMessages();

    if (g_pfnUserCB && (Reason < 1000 || (g_dwSetupFlags & 0x40000))) {
        INT r = (INT)g_pfnUserCB(Reason, MsgType, szMessage, szName, pInfo);
        if (r)
            return r;
    }

    if (Reason == 0x3ed) {
        if (!SetFocus(g_hDlg))
            ERR(0x2ab, "DirectXSetupCallback", "SetFocus()", GetLastError(), "Not fatal...");
        return TRUE;
    }

    if (Reason == 0 || szMessage == NULL)
        return TRUE;

    if (MsgType != 0) {
        char caption[0x104];
        g_pfnXLoadString(STR_ID_CAPTION, caption, 0x104);
        return MessageBoxA(g_hWndParent, szMessage, caption, MsgType);
    }

    if (!g_hDlg) {
        g_hDlg = CreateDialogParamA(g_hInst, MAKEINTRESOURCEA(0x64), g_hWndParent,
                                    ProgressDlgProc, 0);
        if (!g_hDlg) {
            ERR(0x2c3, "DirectXSetupCallback", "CreateDialog()", GetLastError(), NULL);
            return TRUE;
        }
    }
    if (!SetDlgItemTextA(g_hDlg, 0xfa1, szMessage))
        ERR(0x2c6, "DirectXSetupCallback", "SetDlgItemText()", GetLastError(), "Not fatal...");
    return TRUE;
}

INT WINAPI DirectXSetupSetCallback(DSETUP_CALLBACK cb)
{
    if (!g_pfnXSetCallback)
        g_pfnUserCB = cb;
    else
        g_pfnXSetCallback(cb);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* DirectXSetupGetFileVersion                                                */
/* ------------------------------------------------------------------------- */

INT WINAPI DirectXSetupGetFileVersion(LPSTR lpszFile, DWORD *pdwMS, DWORD *pdwLS)
{
    const char *F = "DirectXSetupGetFileVersion";
    DWORD result = 0;

    *pdwMS = 0;
    *pdwLS = 0;

    if (!DXStrNotEqualI(DXFindExtension(DXFindFileName(lpszFile)), "DLS")) {
        /* DirectMusic DLS collection: RIFF 'vers' chunk */
        MMIOINFO info;
        MMCKINFO ckParent, ckChild;
        DWORD    ver[2];
        HMMIO    h;

        memset(&info, 0, sizeof(info));
        h = mmioOpenA(lpszFile, &info, MMIO_READ);
        if (!h)
            return 0;

        memset(&ckParent, 0, sizeof(ckParent));
        if (mmioDescend(h, &ckParent, NULL, MMIO_FINDRIFF) != 0) {
            ERR(0x306, F, "mmioDescend()", 0, NULL);
            if (mmioClose(h, 0) != 0)
                ERR(0x309, F, "mmioClose()", 0, NULL);
            return 0;
        }

        memset(&ckChild, 0, sizeof(ckChild));
        ckChild.ckid = mmioFOURCC('v', 'e', 'r', 's');
        if (mmioDescend(h, &ckChild, &ckParent, MMIO_FINDCHUNK) != 0) {
            ERR(0x315, F, "mmioDescend()", 0, NULL);
            if (mmioClose(h, 0) != 0)
                ERR(0x318, F, "mmioClose()", 0, NULL);
            return 0;
        }

        if (mmioRead(h, (HPSTR)ver, 8) != 8) {
            ERR(0x31f, F, "mmioRead()", 0, NULL);
            if (mmioClose(h, 0) != 0)
                ERR(0x322, F, "mmioClose()", 0, NULL);
            return 0;
        }

        *pdwMS = ver[0];
        *pdwLS = ver[1];
        if (mmioClose(h, 0) != 0)
            ERR(0x32c, F, "mmioClose()", 0, NULL);
        return TRUE;
    } else {
        /* regular PE/resource version */
        DWORD  handle = 0, size;
        LPVOID buf;
        VS_FIXEDFILEINFO *ffi = NULL;
        UINT   len = 0x34;

        size = GetFileVersionInfoSizeA(lpszFile, &handle);
        if (size == 0)
            return 0;

        buf = LocalAlloc(LPTR, size);
        if (!buf) {
            ERR(0x34a, F, "LocalAlloc()", GetLastError(), NULL);
            return 0;
        }
        if (GetFileVersionInfoA(lpszFile, handle, size, buf) &&
            VerQueryValueA(buf, "\\", (LPVOID *)&ffi, &len) && ffi) {
            *pdwMS = ffi->dwFileVersionMS;
            *pdwLS = ffi->dwFileVersionLS;
            result = 1;
        }
        if (LocalFree(buf) != NULL)
            ERR(0x348, F, "LocalFree()", GetLastError(), NULL);
        return (INT)result;
    }
}

/* ------------------------------------------------------------------------- */
/* Forwarders to dsetup32.dll                                                */
/* ------------------------------------------------------------------------- */

INT WINAPI DirectXLoadString(UINT id, LPSTR buf, UINT cch)
{
    if (!g_hDSetup32 && !LoadDSetup32()) {
        ERR(0x20b, "DirectXLoadString", NULL, 0, "LoadDSetup32() failed.");
        return 0;
    }
    return (INT)g_pfnXLoadString(id, buf, cch);
}

INT WINAPI DirectXSetupShowEULA(HWND hWnd)
{
    if (!g_hDSetup32 && !LoadDSetup32()) {
        ERR(0x219, "DirectXSetupShowEULA", NULL, 0, "LoadDSetup32() failed.");
        return 0;
    }
    return g_pfnXShowEULA(hWnd);
}

INT WINAPI DirectXSetupGetEULAA(LPSTR buf, DWORD cb, WORD lang)
{
    if (!g_hDSetup32 && !LoadDSetup32()) {
        ERR(0x225, "DirectXSetupGetEULAA", NULL, 0, "LoadDSetup32() failed.");
        return 0;
    }
    return (INT)g_pfnXGetEULA(buf, cb, lang);
}

INT WINAPI DirectXSetupGetEULAW(LPWSTR buf, DWORD cch, WORD lang)
{
    const char *F = "DirectXSetupGetEULAW";
    INT    result = 0;
    DWORD  need, got;
    char  *mem = NULL;
    int    n;

    if (!g_hDSetup32 && !LoadDSetup32()) {
        ERR(0x235, F, NULL, 0, "LoadDSetup32() failed.");
        return 0;
    }

    need = g_pfnXGetEULA(NULL, 0, lang);
    if (need == 0)
        return 0;

    mem = (char *)malloc(need);
    if (!mem) {
        ERR(0x23f, F, "malloc()", 0, "Unable to allocate memory block.");
        return 0;
    }

    got = g_pfnXGetEULA(mem, need, lang);
    if (got != 0) {
        n = MultiByteToWideChar(0, 0, mem, (int)got, NULL, 0);
        if (n == 0) {
            ERR(0x249, F, "MultiByteToWideChar()", GetLastError(), NULL);
        } else if (cch == 0) {
            result = n;
        } else if (cch < (DWORD)n) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            ERR(0x254, F, NULL, 0, "Insufficient buffer.");
        } else if (buf == NULL) {
            SetLastError(ERROR_INVALID_PARAMETER);
            ERR(0x25c, F, NULL, 0, "Invalid parameter - output buffer is NULL.");
        } else {
            result = MultiByteToWideChar(0, 0, mem, (int)got, buf, (int)cch);
            if (result == 0)
                ERR(0x262, F, "MultiByteToWideChar()", GetLastError(), NULL);
        }
    }
    free(mem);
    return result;
}

/* ------------------------------------------------------------------------- */
/* DirectXSetup / DirectXDeviceDriverSetup                                   */
/* ------------------------------------------------------------------------- */

INT WINAPI DirectXSetupA(HWND hWnd, LPSTR lpszRootPath, DWORD dwFlags)
{
    HANDLE hMutex;
    INT    ret;

    DXSETUP_DPF("DirectXSetupA(): hWnd: %08X dwFlags: %08X", (unsigned)(DWORD_PTR)hWnd, dwFlags);

    hMutex = CreateMutexA(NULL, FALSE, "***DirectXSetupA***");
    if (hMutex && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (dwFlags & 0x1a27)
            dwFlags |= 0x10018;
        g_hWndParent   = hWnd;
        g_dwSetupFlags = dwFlags;
        if (LoadDSetup32())
            ret = g_pfnXSetup(g_hWndParent, lpszRootPath, dwFlags);
        else
            ret = DXSETUP_ERR_BUSY;
    } else {
        ret = DXSETUP_ERR_BUSY;
    }

    if (hMutex && !CloseHandle(hMutex))
        ERR(0x3a2, "DirectXSetupA", "CloseHandle()", GetLastError(), "Not fatal...");

    return ret;
}

/* shared tail of DirectXSetupW (conversion failure) and the DeviceDriver
 * stubs: show the "setup not available" text and tear down the dialog. */
static INT ShowAbortAndDestroyDlg(const char *func, int destroyLine)
{
    char text[0x104];

    g_pfnXLoadString(STR_ID_SETUP_ABORTED, text, 0x104);
    g_pfnXCallback(0xa, 0x10, (DWORD_PTR)text, 0, 0);

    if (g_hDlg) {
        if (!DestroyWindow(g_hDlg))
            ERR(destroyLine, func, "DestroyWindow()", GetLastError(), NULL);
        g_hDlg = NULL;
    }
    return DXSETUP_ERR_BUSY;
}

INT WINAPI DirectXSetupW(HWND hWnd, LPWSTR lpszRootPath, DWORD dwFlags)
{
    char root[0x104];

    DXSETUP_DPF("DirectXSetupW()");

    if (lpszRootPath == NULL || lpszRootPath[0] == 0)
        return DirectXSetupA(hWnd, NULL, dwFlags);

    if (WideCharToMultiByte(0, 0, lpszRootPath, -1, root, 0x104, NULL, NULL) != 0)
        return DirectXSetupA(hWnd, root, dwFlags);

    ERR(0x3b5, "DirectXSetupW", "WideCharToMultiByte()", GetLastError(), NULL);

    if (!g_hDSetup32 && !LoadDSetup32()) {
        ERR(0x3b8, "DirectXSetupW", NULL, 0, "LoadDSetup32() failed.");
        return DXSETUP_ERR_BUSY;
    }
    g_hWndParent   = hWnd;
    g_dwSetupFlags = dwFlags;
    return ShowAbortAndDestroyDlg("DirectXSetupW", 0x3c9);
}

INT WINAPI DirectXDeviceDriverSetupA(HWND hWnd, LPSTR lpszDriverClass,
                                     LPSTR lpszDriverPath, DWORD dwFlags)
{
    (void)lpszDriverClass; (void)lpszDriverPath;
    DXSETUP_DPF("DirectXDeviceDriverSetupA()");
    g_dwSetupFlags = dwFlags;
    g_hWndParent   = hWnd;
    /* NOTE: the original dereferences the dsetup32 pointers without loading
     * the DLL first. Guard against that instead of crashing. */
    if (!g_pfnXLoadString || !g_pfnXCallback)
        return DXSETUP_ERR_BUSY;
    return ShowAbortAndDestroyDlg("DirectXDeviceDriverSetupA", 0x3f6);
}

INT WINAPI DirectXDeviceDriverSetupW(HWND hWnd, LPWSTR lpszDriverClass,
                                     LPWSTR lpszDriverPath, DWORD dwFlags)
{
    (void)lpszDriverClass; (void)lpszDriverPath;
    DXSETUP_DPF("DirectXDeviceDriverSetupW()");
    g_dwSetupFlags = dwFlags;
    g_hWndParent   = hWnd;
    if (!g_pfnXLoadString || !g_pfnXCallback)
        return DXSETUP_ERR_BUSY;
    return ShowAbortAndDestroyDlg("DirectXDeviceDriverSetupW", 0x413);
}

/* ------------------------------------------------------------------------- */
/* DirectPlay lobby application registration                                 */
/* ------------------------------------------------------------------------- */

static BOOL ValidateDXRegAppStructA(const DIRECTXREGISTERAPPLICATIONA *p)
{
    const char *F = "ValidateDXRegAppStructA";
    DXSETUP_DPF("ValidateDXRegAppStructA:");

    if (!p)                         { ERR(0x7a7, F, NULL, 0, "Invalid parameter - structure pointer is NULL."); return FALSE; }
    if (p->dwSize != 0x20 && p->dwSize != 0x24)
                                    { ERR(0x7ad, F, NULL, 0, "Invalid structure member - structure size is incorrect."); return FALSE; }
    if (!p->lpszApplicationName)    { ERR(0x7b3, F, NULL, 0, "Invalid structure member - lpszApplicationName is NULL."); return FALSE; }
    if (!p->lpszFilename)           { ERR(0x7b9, F, NULL, 0, "Invalid structure member - lpszFilename is NULL."); return FALSE; }
    if (!p->lpszCommandLine)        { ERR(0x7bf, F, NULL, 0, "Invalid structure member - lpszCommandLine is NULL."); return FALSE; }
    if (!p->lpszPath)               { ERR(0x7c5, F, NULL, 0, "Invalid structure member - lpszPath is NULL."); return FALSE; }
    if (!p->lpszCurrentDirectory)   { ERR(0x7cb, F, NULL, 0, "Invalid structure member - lpszCurrentDirectory is NULL."); return FALSE; }

    DXSETUP_DPF("ValidateDXRegAppStructA:  Valid struct");
    return TRUE;
}

static BOOL ValidateDXRegAppStructW(const DIRECTXREGISTERAPPLICATIONW *p)
{
    const char *F = "ValidateDXRegAppStructW";
    DXSETUP_DPF("ValidateDXRegAppStructW:");

    if (!p)                         { ERR(0x7f8, F, NULL, 0, "Invalid parameter - structure pointer is NULL."); return FALSE; }
    /* QUIRK: the W validator accepts 0x20 or 4 (not 0x24). */
    if (p->dwSize != 0x20 && p->dwSize != 4)
                                    { ERR(0x7fe, F, NULL, 0, "Invalid structure member - structure size is incorrect."); return FALSE; }
    if (!p->lpszApplicationName)    { ERR(0x804, F, NULL, 0, "Invalid structure member - lpszApplicationName is NULL."); return FALSE; }
    if (!p->lpszFilename)           { ERR(0x80a, F, NULL, 0, "Invalid structure member - lpszFilename is NULL."); return FALSE; }
    if (!p->lpszCommandLine)        { ERR(0x810, F, NULL, 0, "Invalid structure member - lpszCommandLine is NULL."); return FALSE; }
    if (!p->lpszPath)               { ERR(0x816, F, NULL, 0, "Invalid structure member - lpszPath is NULL."); return FALSE; }
    if (!p->lpszCurrentDirectory)   { ERR(0x81c, F, NULL, 0, "Invalid structure member - lpszCurrentDirectory is NULL."); return FALSE; }

    DXSETUP_DPF("ValidateDXRegAppStructW:  Valid struct");
    return TRUE;
}

/* --- ANSI --- */

/* Load string `id` from dsetup32 and write REG_SZ `data` under that name. */
static BOOL RegSetA(HKEY hKey, UINT id, const char *data, int idLine, int setLine)
{
    const char *F = "DirectXRegisterApplicationA";
    char  name[0x104];
    LONG  r;

    if (!g_pfnXLoadString(id, name, 0x104)) {
        ERR(idLine, F, NULL, 0, "XDirectXLoadString() failed.");
        return FALSE;
    }
    r = RegSetValueExA(hKey, name, 0, REG_SZ, (const BYTE *)data, (DWORD)lstrlenA(data) + 1);
    if (r != ERROR_SUCCESS) {
        ERR(setLine, F, "RegSetValueExA()", r, NULL);
        return FALSE;
    }
    return TRUE;
}

INT WINAPI DirectXRegisterApplicationA(HWND hWnd, LPDIRECTXREGISTERAPPLICATIONA app)
{
    const char *F = "DirectXRegisterApplicationA";
    char   fmt[0x104], path[0x104], guidA[0x104];
    WCHAR  guidW[0x104];
    HKEY   hKey = NULL;
    DWORD  disp;
    LONG   r;
    int    n;
    (void)hWnd;

    DXSETUP_DPF("DirectXRegisterApplicationA");

    if (!IsAdmin()) {
        ERR(0x43c, F, NULL, 0, "Current logon user doesn't have the Administrator privilege.");
        return 0;
    }
    if (!g_hDSetup32 && !LoadDSetup32()) {
        ERR(0x444, F, NULL, 0, "LoadDSetup32() failed.");
        return 0;
    }
    if (!app) {
        ERR(0x44b, F, NULL, 0, "Invalid parameter - struct pointer is NULL.");
        return 0;
    }
    if (!ValidateDXRegAppStructA(app))
        return 0;
    if (!g_pfnXLoadString) {
        ERR(0x458, F, NULL, 0, "Function pointer XDirectXLoadString is not initialized.");
        return 0;
    }
    if (!g_pfnXLoadString(STR_ID_APPKEY_FMT, fmt, 0x104)) {
        ERR(0x45e, F, NULL, 0, "XDirectXLoadString() failed.");
        return 0;
    }
    if (lstrlenA(app->lpszApplicationName) + lstrlenA(fmt) - 1 > 0x104) {
        ERR(0x463, F, NULL, 0, "Application name is too long, %s.", app->lpszApplicationName);
        return 0;
    }
    StrPrintfA(path, 0x104, fmt, app->lpszApplicationName);

    r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, &disp);
    if (r != ERROR_SUCCESS) {
        ERR(0x473, F, "RegCreateKeyExA()", r, NULL);
        goto fail;
    }

    n = StringFromGUID2(app->lpGUID, guidW, 0x104);
    if (!n) {
        ERR(0x47d, F, "StringFromGUID2()", 0, NULL);
        goto fail;
    }
    /* The original passes the StringFromGUID2 return value (chars incl. NUL)
     * as cchWideChar. */
    n = WideCharToMultiByte(0, 0, guidW, n, guidA, 0x104, NULL, NULL);
    if (!n) {
        ERR(0x486, F, "WideCharToMultiByte()", GetLastError(), "Unable to convert GUID from UNICODE to ANSI.");
        goto fail;
    }

    if (!RegSetA(hKey, STR_ID_GUID,        guidA,                     0x48c, 0x499)) goto fail;
    if (!RegSetA(hKey, STR_ID_FILE,        app->lpszFilename,         0x4a2, 0x4af)) goto fail;
    if (!RegSetA(hKey, STR_ID_COMMANDLINE, app->lpszCommandLine,      0x4b8, 0x4c5)) goto fail;
    if (!RegSetA(hKey, STR_ID_PATH,        app->lpszPath,             0x4ce, 0x4db)) goto fail;
    if (!RegSetA(hKey, STR_ID_CURRENTDIR,  app->lpszCurrentDirectory, 0x4e4, 0x4f1)) goto fail;

    /* QUIRK: tests dwSize == 4 (never true after the A validator). */
    if (app->dwSize == 4 && app->lpszLauncherName) {
        if (!RegSetA(hKey, STR_ID_LAUNCHER, app->lpszLauncherName, 0x4fc, 0x509)) goto fail;
    }

    r = RegCloseKey(hKey);
    if (r != ERROR_SUCCESS)
        ERR(0x50f, F, "RegCloseKey()", r, NULL);
    DXSETUP_DPF("DirectXRegisterApplicationA: Completed.");
    return TRUE;

fail:
    if (hKey) {
        r = RegCloseKey(hKey);
        if (r != ERROR_SUCCESS)
            ERR(0x518, F, "RegCloseKey() failed", r, NULL);
    }
    DXSETUP_DPF("DirectXRegisterApplicationA: ERROR!");
    return 0;
}

/* --- Unicode --- */

/* 0x40574b */
static BOOL MyLoadStringW(HINSTANCE hInst, UINT id, LPWSTR buf, int cch)
{
    char a[0x104];
    int  need;
    (void)hInst;

    if (!g_hDSetup32 && !LoadDSetup32()) {
        ERR(0x52e, "MyLoadStringW", NULL, 0, "LoadDSetup32() failed.");
        return FALSE;
    }
    if (!g_pfnXLoadString(id, a, 0x104)) {
        ERR(0x535, "MyLoadStringW", NULL, 0, "XDirectXLoadString() failed.");
        return FALSE;
    }
    need = MultiByteToWideChar(0, 0, a, -1, NULL, 0);
    if (need > cch) {
        ERR(0x53b, "MyLoadStringW", NULL, 0, "Insufficient buffer.");
        return FALSE;
    }
    if (!MultiByteToWideChar(0, 0, a, -1, buf, cch)) {
        ERR(0x541, "MyLoadStringW", "MultiByteToWideChar()", GetLastError(), NULL);
        return FALSE;
    }
    return TRUE;
}

static BOOL RegSetW(HKEY hKey, UINT id, const WCHAR *data, int idLine, int setLine)
{
    const char *F = "DirectXRegisterApplicationW";
    WCHAR name[0x104];
    LONG  r;

    if (!MyLoadStringW(g_hInst, id, name, 0x104)) {
        ERR(idLine, F, NULL, 0, id == STR_ID_COMMANDLINE ? "MyLoadStringW()." : "MyLoadStringW() failed.");
        return FALSE;
    }
    r = RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE *)data,
                       (DWORD)(lstrlenW(data) * 2 + 2));
    if (r != ERROR_SUCCESS) {
        ERR(setLine, F, "RegSetValueExW()", r, NULL);
        return FALSE;
    }
    return TRUE;
}

INT WINAPI DirectXRegisterApplicationW(HWND hWnd, LPDIRECTXREGISTERAPPLICATIONW app)
{
    const char *F = "DirectXRegisterApplicationW";
    OSVERSIONINFOA vi;
    WCHAR  fmt[0x104], path[0x104], guidW[0x104];
    HKEY   hKey = NULL;
    DWORD  disp;
    LONG   r;
    (void)hWnd;

    DXSETUP_DPF("DirectXRegisterApplicationW:");

    if (!IsAdmin()) {
        ERR(0x55a, F, NULL, 0, "Current logon user doesn't have the Administrator privilege.");
        return 0;
    }

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = 0x94;
    if (!GetVersionExA(&vi)) {
        ERR(0x563, F, " GetVersionEx()", GetLastError(), NULL);
        return 0;
    }
    if (vi.dwPlatformId != VER_PLATFORM_WIN32_NT) {
        ERR(0x571, F, NULL, 0, "Not running NT. Platform ID: [%08X]", vi.dwPlatformId);
        return 0;
    }
    if (vi.dwMajorVersion < 4) {
        ERR(0x577, F, NULL, 0, "Not running NT 4.0. Version: [%08X]", vi.dwMajorVersion);
        return 0;
    }
    if (!app) {
        ERR(0x581, F, NULL, 0, "Invalid parameter - struct pointer is NULL.");
        return 0;
    }
    if (!ValidateDXRegAppStructW(app))
        return 0;

    if (!MyLoadStringW(g_hInst, STR_ID_APPKEY_FMT, fmt, 0x104)) {
        ERR(0x591, F, NULL, 0, "MyLoadStringW() failed.");
        return 0;
    }
    if (((lstrlenW(app->lpszApplicationName) + lstrlenW(fmt) - 2) * 2) > 0x208) {
        ERR(0x596, F, NULL, 0, "Application name is too long.");
        return 0;
    }
    StrPrintfW(path, 0x104, fmt, app->lpszApplicationName);

    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, &disp);
    if (r != ERROR_SUCCESS) {
        ERR(0x5a7, F, "RegCreateKeyExW()", r, NULL);
        goto fail;
    }
    if (!StringFromGUID2(app->lpGUID, guidW, 0x104)) {
        ERR(0x5b1, F, "StringFromGUID2()", 0, NULL);
        goto fail;
    }

    if (!RegSetW(hKey, STR_ID_GUID,        guidW,                     0x5b7, 0x5c4)) goto fail;
    if (!RegSetW(hKey, STR_ID_FILE,        app->lpszFilename,         0x5cd, 0x5da)) goto fail;
    if (!RegSetW(hKey, STR_ID_COMMANDLINE, app->lpszCommandLine,      0x5e3, 0x5f0)) goto fail;
    if (!RegSetW(hKey, STR_ID_PATH,        app->lpszPath,             0x5f9, 0x606)) goto fail;
    if (!RegSetW(hKey, STR_ID_CURRENTDIR,  app->lpszCurrentDirectory, 0x60f, 0x61c)) goto fail;

    /* QUIRK: dwSize == 4 test, and lpszLauncherName read from +0x20. */
    if (app->dwSize == 4 && app->lpszLauncherName) {
        if (!RegSetW(hKey, STR_ID_LAUNCHER, app->lpszLauncherName, 0x628, 0x635)) goto fail;
    }

    r = RegCloseKey(hKey);
    if (r != ERROR_SUCCESS)
        ERR(0x63b, F, "RegCloseKey()", r, NULL);
    DXSETUP_DPF("DirectXRegisterApplicationW: Completed.");
    return TRUE;

fail:
    if (hKey) {
        r = RegCloseKey(hKey);
        if (r != ERROR_SUCCESS)
            ERR(0x645, F, "RegCloseKey()", r, NULL);
    }
    DXSETUP_DPF("DirectXRegisterApplicationW: ERROR!");
    return 0;
}

/* --- Unregister --- */

INT WINAPI DirectXUnRegisterApplication(HWND hWnd, LPGUID lpGUID)
{
    const char *F = "DirectXUnRegisterApplication";
    HKEY   hApps = NULL, hApp = NULL;
    WCHAR  guidW[0x104];
    char   guidA[0x104];
    char   name[0x104];
    char   val[0x104];
    char   cls[0x200];
    DWORD  cchClass = sizeof(cls), cSubKeys = 0, maxSub, maxCls, cVals, maxVN, maxVL, secLen;
    FILETIME ft;
    DWORD  idx = 0, cchName, type, cb;
    BOOL   found = FALSE;
    LONG   r;
    int    n;
    (void)hWnd;

    DXSETUP_DPF("DirectXUnRegisterApplication:");

    if (!IsAdmin()) {
        ERR(0x675, F, NULL, 0, "Current logon user doesn't have the Administrator privilege.");
        return 0;
    }
    if (!lpGUID) {
        ERR(0x67e, F, NULL, 0, "Invalid parameter - GUID is NULL.");
        goto fail;
    }

    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\DirectPlay\\Applications",
                      0, KEY_ALL_ACCESS, &hApps);
    if (r != ERROR_SUCCESS) {
        ERR(0x68c, F, "RegOpenKeyEx()", r, NULL);
        goto fail;
    }

    n = StringFromGUID2(lpGUID, guidW, 0x104);
    if (!n) {
        ERR(0x697, F, "StringFromGUID2()", 0, NULL);
        goto fail;
    }
    if (!WideCharToMultiByte(0, 0, guidW, n, guidA, 0x104, NULL, NULL)) {
        ERR(0x6a0, F, "WideCharToMultiByte()", GetLastError(), "Unable to convert GUID from UNICODE to ANSI.");
        goto fail;
    }

    r = RegQueryInfoKeyA(hApps, cls, &cchClass, NULL, &cSubKeys, &maxSub, &maxCls,
                         &cVals, &maxVN, &maxVL, &secLen, &ft);
    if (r != ERROR_SUCCESS) {
        ERR(0x6b6, F, "RegQueryInfoKey()", r, NULL);
        goto fail;
    }

    while (idx < cSubKeys) {
        cchName = 0x104;
        r = RegEnumKeyExA(hApps, idx, name, &cchName, NULL, NULL, NULL, &ft);
        if (r != ERROR_SUCCESS) {
            ERR(0x6d2, F, "RegEnumKeyEx()", r, "Index: %d", (int)idx);
            goto fail;
        }
        r = RegOpenKeyExA(hApps, name, 0, KEY_QUERY_VALUE, &hApp);
        if (r != ERROR_SUCCESS) {
            ERR(0x6e1, F, "RegOpenKeyEx()", r, "App: ", name);
            goto fail;
        }
        type = REG_SZ;
        cb = 0x104;
        r = RegQueryValueExA(hApp, "Guid", NULL, &type, (LPBYTE)val, &cb);
        if (r != ERROR_SUCCESS) {
            ERR(0x6f3, F, "RegQueryValueEx()", r, NULL);
            goto fail;
        }
        r = RegCloseKey(hApp);
        if (r != ERROR_SUCCESS)
            ERR(0x6f8, F, "RegCloseKey()", r, NULL);

        if (lstrcmpA(guidA, val) == 0)
            found = TRUE;
        idx++;
        if (found) break;
    }

    if (!found) {
        ERR(0x70a, F, NULL, 0, "Unable to find GUID.");
        goto fail;
    }

    r = RegDeleteKeyA(hApps, name);
    if (r != ERROR_SUCCESS) {
        ERR(0x714, F, "RegDeleteKey()", r, "Unable to delete key %s.", name);
        goto fail;
    }

    r = RegCloseKey(hApps);
    if (r != ERROR_SUCCESS)
        ERR(0x71c, F, "RegCloseKey()", r, NULL);
    DXSETUP_DPF("DirectXUnRegisterApplication: Completed.");
    return TRUE;

fail:
    if (hApps) {
        r = RegCloseKey(hApps);
        if (r != ERROR_SUCCESS)
            ERR(0x728, F, "RegCloseKey()", r, NULL);
    }
    if (hApp) {
        /* QUIRK: handle may already have been closed in the loop above. */
        r = RegCloseKey(hApp);
        if (r != ERROR_SUCCESS)
            ERR(0x72e, F, "RegCloseKey()", r, NULL);
    }
    DXSETUP_DPF("DirectXUnRegisterApplication: ERROR!");
    return 0;
}

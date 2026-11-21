/*
 * dsetup.h - public types for the dsetup.dll re-implementation.
 *
 * Layouts follow the (public) DirectX SDK "dsetup.h" contract that the
 * original dsetup.dll (DirectX 9.0c, file version 4.9.0.0904) exposes.
 */
#ifndef DSETUP_REIMPL_H
#define DSETUP_REIMPL_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback given to DirectXSetupSetCallback / forwarded to dsetup32.dll */
typedef DWORD (WINAPI *DSETUP_CALLBACK)(DWORD Reason, DWORD MsgType,
                                        LPSTR szMessage, LPSTR szName,
                                        void *pInfo);

/* DirectXRegisterApplication{A,W} argument. dwSize is validated by the
 * Validate* helpers (see dsetup.c). lpszLauncherName only exists in the
 * extended layout, but the original reads it unconditionally when
 * dwSize == 4, so it is part of the internal view of the structure. */
typedef struct _DIRECTXREGISTERAPPLICATIONA {
    DWORD  dwSize;
    DWORD  dwFlags;
    LPSTR  lpszApplicationName;
    LPGUID lpGUID;
    LPSTR  lpszFilename;
    LPSTR  lpszCommandLine;
    LPSTR  lpszPath;
    LPSTR  lpszCurrentDirectory;
    LPSTR  lpszLauncherName;
} DIRECTXREGISTERAPPLICATIONA, *PDIRECTXREGISTERAPPLICATIONA,
  *LPDIRECTXREGISTERAPPLICATIONA;

typedef struct _DIRECTXREGISTERAPPLICATIONW {
    DWORD   dwSize;
    DWORD   dwFlags;
    LPWSTR  lpszApplicationName;
    LPGUID  lpGUID;
    LPWSTR  lpszFilename;
    LPWSTR  lpszCommandLine;
    LPWSTR  lpszPath;
    LPWSTR  lpszCurrentDirectory;
    LPWSTR  lpszLauncherName;
} DIRECTXREGISTERAPPLICATIONW, *PDIRECTXREGISTERAPPLICATIONW,
  *LPDIRECTXREGISTERAPPLICATIONW;

/* ---- exports (ordinal in comment) ---- */
INT   WINAPI DirectXDeviceDriverSetupA(HWND, LPSTR, LPSTR, DWORD);           /*  1 */
INT   WINAPI DirectXDeviceDriverSetupW(HWND, LPWSTR, LPWSTR, DWORD);         /*  2 */
INT   WINAPI DirectXRegisterApplicationA(HWND, LPDIRECTXREGISTERAPPLICATIONA);/*  3 */
INT   WINAPI DirectXRegisterApplicationW(HWND, LPDIRECTXREGISTERAPPLICATIONW);/*  4 */
INT   WINAPI DirectXSetupA(HWND, LPSTR, DWORD);                              /*  5 */
INT   WINAPI DirectXSetupIsJapan(void);                                      /*  6 */
INT   WINAPI DirectXSetupIsJapanNec(void);                                   /*  7 */
INT   WINAPI DirectXSetupW(HWND, LPWSTR, DWORD);                             /*  8 */
INT   WINAPI DirectXUnRegisterApplication(HWND, LPGUID);                     /*  9 */
INT   WINAPI DirectXSetupSetCallback(DSETUP_CALLBACK);                       /* 10 */
INT   WINAPI DirectXSetupGetVersion(DWORD *, DWORD *);                       /* 11 */
INT   WINAPI DirectXSetupCallback(DWORD, DWORD, LPSTR, LPSTR, void *);       /* 12 */
INT   WINAPI DirectXSetupGetFileVersion(LPSTR, DWORD *, DWORD *);            /* 13 */
INT   WINAPI DirectXLoadString(UINT, LPSTR, UINT);                           /* 14 */
INT   WINAPI DirectXSetupIsEng(void);                                        /* 15 */
INT   WINAPI DirectXSetupShowEULA(HWND);                                     /* 16 */
INT   WINAPI DirectXSetupGetEULAA(LPSTR, DWORD, WORD);                       /* 17 */
INT   WINAPI DirectXSetupGetEULAW(LPWSTR, DWORD, WORD);                      /* 18 */

#ifdef __cplusplus
}
#endif
#endif

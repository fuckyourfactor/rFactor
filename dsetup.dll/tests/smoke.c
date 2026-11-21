#include <windows.h>
#include <stdio.h>
typedef int (WINAPI *F0)(void);
typedef int (WINAPI *FV)(DWORD*,DWORD*);
typedef int (WINAPI *FF)(LPSTR,DWORD*,DWORD*);
int main(void){
  HMODULE h=LoadLibraryA("dsetup.dll"); if(!h){printf("load fail %lu\n",GetLastError());return 1;}
  DWORD v=0,r=0,ms=0,ls=0;
  printf("IsEng=%d IsJapan=%d Nec=%d\n",((F0)GetProcAddress(h,"DirectXSetupIsEng"))(),((F0)GetProcAddress(h,"DirectXSetupIsJapan"))(),((F0)GetProcAddress(h,"DirectXSetupIsJapanNec"))());
  ((FV)GetProcAddress(h,(LPCSTR)11))(&v,&r); printf("GetVersion %08lx %08lx\n",v,r); if(!v) return 2;
  int ok=((FF)GetProcAddress(h,"DirectXSetupGetFileVersion"))("dsetup.dll",&ms,&ls); printf("FileVer ok=%d %08lx %08lx\n",ok,ms,ls); if(!ok||ms!=0x40009||ls!=0x388) return 3;
  ok=((FF)GetProcAddress(h,"DirectXSetupGetFileVersion"))("nonexist.dll",&ms,&ls); printf("FileVer(nonexist) ok=%d\n",ok);
  /* callback: no dsetup32 -> LoadString should fail gracefully */
  typedef int (WINAPI *FL)(UINT,LPSTR,UINT); char b[100]; printf("LoadString=%d\n",((FL)GetProcAddress(h,"DirectXLoadString"))(1,b,100));
  FreeLibrary(h); return 0;}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllexport) int __cdecl fixture_emit(void) {
    const unsigned char poll[]={0xE0,5,0,1,0x42,0,0x48};
    const unsigned char on[]={0xE0,5,0,1,0x40,0,0x46};
    const unsigned char off[]={0xE0,5,0,1,0x41,0,0x47};
    HANDLE file=CreateFileA("late-stimulus.bin",GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD written;
    int index,ok=1;
    if (file==INVALID_HANDLE_VALUE) return 0;
    for (index=0;index<3;index++) {
        if (!WriteFile(file,on,sizeof(on),&written,NULL) || written!=sizeof(on)) ok=0;
        if (!WriteFile(file,poll,sizeof(poll),&written,NULL) || written!=sizeof(poll)) ok=0;
        if (!WriteFile(file,off,sizeof(off),&written,NULL) || written!=sizeof(off)) ok=0;
    }
    CloseHandle(file);
    return ok;
}

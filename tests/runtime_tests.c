#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned port;
static int failures;
static LONG chained_calls;
typedef BOOL (WINAPI *write_proc)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static write_proc original_write;
static BOOL WINAPI existing_write(HANDLE file,LPCVOID bytes,DWORD length,LPDWORD written,LPOVERLAPPED overlap) {
    InterlockedIncrement(&chained_calls);
    return original_write(file,bytes,length,written,overlap);
}
static int install_existing_hook(void) {
    BYTE *base=(BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt=(IMAGE_NT_HEADERS *)(base+dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *descriptor=(IMAGE_IMPORT_DESCRIPTOR *)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (;descriptor->Name;descriptor++) {
        IMAGE_THUNK_DATA *names=(IMAGE_THUNK_DATA *)(base+descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA *targets=(IMAGE_THUNK_DATA *)(base+descriptor->FirstThunk);
        if (!descriptor->OriginalFirstThunk) continue;
        for (;names->u1.AddressOfData;names++,targets++) {
            DWORD protection,ignored;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base+names->u1.AddressOfData))->Name,"WriteFile")) continue;
            original_write=(write_proc)(ULONG_PTR)targets->u1.Function;
            if (!VirtualProtect(&targets->u1.Function,sizeof(PVOID),PAGE_READWRITE,&protection)) return 0;
            InterlockedExchangePointer((PVOID volatile *)&targets->u1.Function,(PVOID)existing_write);
            VirtualProtect(&targets->u1.Function,sizeof(PVOID),protection,&ignored);
            return 1;
        }
    }
    return 0;
}
static char response[4096];
static void check(int condition,const char *name) {
    if (!condition) { printf("FAIL: %s response=%s\n",name,response); failures++; }
}
static int request(const char *method,const char *path) {
    SOCKET client;
    struct sockaddr_in endpoint;
    char outgoing[512];
    int total=0,received;
    DWORD timeout=2000;
    response[0]=0;
    client=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    memset(&endpoint,0,sizeof(endpoint));
    endpoint.sin_family=AF_INET;
    endpoint.sin_port=htons((u_short)port);
    endpoint.sin_addr.s_addr=inet_addr("127.0.0.1");
    setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof(timeout));
    if (connect(client,(struct sockaddr *)&endpoint,sizeof(endpoint))) { closesocket(client); return 0; }
    snprintf(outgoing,sizeof(outgoing),"%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",method,path);
    send(client,outgoing,(int)strlen(outgoing),0);
    while (total<(int)sizeof(response)-1 && (received=recv(client,response+total,sizeof(response)-1-total,0))>0) total+=received;
    response[total]=0;
    closesocket(client);
    return total>0;
}
static void emit(HANDLE file,const unsigned char *bytes,DWORD length) {
    DWORD written=0;
    check(WriteFile(file,bytes,length,&written,NULL) && written==length,"WriteFile stimulus");
}
int main(int argc,char **argv) {
    WSADATA winsock;
    HMODULE module;
    HANDLE file;
    int index;
    unsigned occupied_timeout;
    char expected_timeout[80];
    const unsigned char poll[]={0xE0,5,0,1,0x42,0,0x48};
    const unsigned char card[]={0xE0,9,0,1,0x43,4,1,2,3,4,0x5B};
    const unsigned char off[]={0xE0,5,0,1,0x41,0,0x47};
    const unsigned char on[]={0xE0,5,0,1,0x40,0,0x46};
    if (argc!=5) return 2;
    port=(unsigned)atoi(argv[2]);
    occupied_timeout=(unsigned)atoi(argv[4]);
    snprintf(expected_timeout,sizeof(expected_timeout),"\"occupied_timeout_seconds\":%u",occupied_timeout);
    WSAStartup(MAKEWORD(2,2),&winsock);
    check(install_existing_hook(),"install simulated segatools hook");
    module=LoadLibraryA(argv[1]);
    if (!module) { printf("LOAD_FAILED=%lu\n",(unsigned long)GetLastError()); return 1; }
    printf("LOAD_SUCCEEDED bits=%u\n",(unsigned)(sizeof(void *)*8));
    for (index=0;index<40;index++) { if (request("GET","/debug")) break; Sleep(100); }
    check(index<40 && strstr(response,"\"hooks\":"),"debug endpoint ready");
    check(strstr(response,expected_timeout)!=NULL,"configured occupied timeout matches policy");
    check(request("GET","/status") && strstr(response,"503 Service Unavailable"),"initial unknown");
    check(request("GET","/missing") && strstr(response,"404 Not Found"),"404");
    check(request("POST","/status") && strstr(response,"405 Method Not Allowed"),"405");
    check(request("HEAD","/debug") && strstr(response,"\r\n\r\n") && strlen(strstr(response,"\r\n\r\n"))==4,"HEAD has no body");
    {
        HMODULE late=LoadLibraryA(argv[3]);
        typedef int (__cdecl *fixture_proc)(void);
        fixture_proc emit_fixture=late ? (fixture_proc)GetProcAddress(late,"fixture_emit") : NULL;
        if (late && !emit_fixture) emit_fixture=(fixture_proc)GetProcAddress(late,"_fixture_emit");
        check(emit_fixture!=NULL,"load late module");
        Sleep(3400);
        if (emit_fixture) check(emit_fixture(),"late module writes");
        check(request("GET","/status") && strstr(response,"\"status\":\"free\""),"late loaded module interleaved radio cycles establish idle");
        check(request("GET","/debug") && strstr(response,"\"radio_on_requests\":3") && strstr(response,"\"radio_off_requests\":3") && strstr(response,"\"polls\":3"),"radio command diagnostics");
    }
    file=CreateFileA("test-stimulus.bin",GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    check(file!=INVALID_HANDLE_VALUE,"open fixture");
    emit(file,poll,3); emit(file,poll+3,sizeof(poll)-3);
    emit(file,poll,sizeof(poll)); emit(file,poll,sizeof(poll));
    check(request("GET","/status?query=1") && strstr(response,"\"status\":\"free\""),"IAT hook real polls and split writes");
    emit(file,card,sizeof(card));
    check(request("GET","/status") && strstr(response,"\"status\":\"occupied\""),"IAT hook card");
    Sleep(1100);
    for (index=0;index<9;index++) {
        emit(file,on,sizeof(on));
        emit(file,poll,sizeof(poll));
        emit(file,off,sizeof(off));
        Sleep(220);
    }
    check(request("GET","/status") && strstr(response,"\"status\":\"free\""),"idle resumes");
    emit(file,off,sizeof(off));
    Sleep(2400);
    check(request("GET","/status") && strstr(response,"\"status\":\"occupied\""),"guest timer without HTTP traffic");
    Sleep(2000);
    if (occupied_timeout) {
        check(request("GET","/status") && strstr(response,"503 Service Unavailable"),"optional occupied expiry remains supported");
        check(request("GET","/debug") && strstr(response,"occupied_signal_expired"),"debug explains optional expiry");
    } else {
        check(request("GET","/status") && strstr(response,"\"status\":\"occupied\""),"continued play remains occupied without new signals");
        for (index=0;index<3;index++) { emit(file,poll,sizeof(poll)); Sleep(220); }
        check(request("GET","/status") && strstr(response,"\"status\":\"occupied\""),"brief polling does not release long occupied session");
        for (index=0;index<6;index++) { emit(file,poll,sizeof(poll)); Sleep(220); }
        check(request("GET","/status") && strstr(response,"\"status\":\"free\""),"confirmed polling releases continued session");
        emit(file,card,sizeof(card));
        check(request("GET","/status") && strstr(response,"\"status\":\"occupied\""),"second card session enters occupied");
        check(request("GET","/debug") && strstr(response,expected_timeout),"debug keeps disabled expiry policy");
    }
    check(InterlockedCompareExchange(&chained_calls,0,0)>=15,"pre-existing WriteFile hook preserved");
    printf("FINAL_DEBUG=%s\n",response);
    CloseHandle(file);
    printf("RUNTIME_TEST_FAILURES=%d\n",failures);
    return failures ? 1 : 0;
}

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "cabinet_core.h"

#define MODULE_LIMIT 32
#define STREAM_LIMIT 32
typedef BOOL (WINAPI *write_proc)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef int (WSAAPI *send_proc)(SOCKET,const char *,int,int);
typedef int (WSAAPI *wsasend_proc)(SOCKET,LPWSABUF,DWORD,LPDWORD,DWORD,LPWSAOVERLAPPED,LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef HANDLE (WINAPI *request_proc)(HANDLE,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR *,DWORD);
typedef struct { HMODULE module; PVOID original[4]; unsigned mask; } module_slot;
typedef struct { HANDLE handle; cab_decoder decoder; uint64_t touched; } stream_slot;
static HMODULE self_module;
static CRITICAL_SECTION state_lock;
static cab_state state;
static cab_config config;
static module_slot modules[MODULE_LIMIT];
static stream_slot streams[STREAM_LIMIT];
static HANDLE log_file = INVALID_HANDLE_VALUE;
static wchar_t ini_path[MAX_PATH];
static wchar_t log_path[MAX_PATH];
static volatile LONG ready;
static LONG write_calls, network_calls, hook_count;
static uint32_t logged_revision;
static LARGE_INTEGER clock_frequency;
static uint64_t now_ms(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart / clock_frequency.QuadPart) * 1000 +
           (uint64_t)(counter.QuadPart % clock_frequency.QuadPart) * 1000 / clock_frequency.QuadPart;
}
static void log_line(const char *format, ...) {
    char message[1400], line[1600];
    SYSTEMTIME stamp;
    DWORD written;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    message[sizeof(message)-1] = 0;
    GetLocalTime(&stamp);
    snprintf(line,sizeof(line),"%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
             stamp.wYear,stamp.wMonth,stamp.wDay,stamp.wHour,stamp.wMinute,stamp.wSecond,message);
    line[sizeof(line)-1] = 0;
    if (log_file != INVALID_HANDLE_VALUE) {
        WriteFile(log_file,line,(DWORD)strlen(line),&written,NULL);
        FlushFileBuffers(log_file);
    }
    OutputDebugStringA(line);
}
static void log_transition(void) {
    if (state.revision != logged_revision) {
        logged_revision = state.revision;
        log_line("state=%s reason=%s revision=%lu",cab_phase_name(state.phase),state.reason,(unsigned long)state.revision);
    }
}
static void observe_write(HANDLE handle, const void *bytes, DWORD length) {
    unsigned index, selected = 0;
    uint64_t timestamp;
    if (!InterlockedCompareExchange(&ready,0,0) || !bytes || !length || handle == log_file) return;
    timestamp=now_ms();
    InterlockedIncrement(&write_calls);
    EnterCriticalSection(&state_lock);
    for (index=0; index<STREAM_LIMIT; index++) {
        if (streams[index].handle == handle) { selected=index; break; }
        if (!streams[index].handle || streams[index].touched < streams[selected].touched) selected=index;
    }
    if (index == STREAM_LIMIT) {
        memset(&streams[selected],0,sizeof(streams[selected]));
        streams[selected].handle=handle;
    }
    if (timestamp-streams[selected].touched>2000) memset(&streams[selected].decoder,0,sizeof(streams[selected].decoder));
    streams[selected].touched=timestamp;
    cab_feed(&state,&config,&streams[selected].decoder,(const uint8_t *)bytes,length,timestamp);
    log_transition();
    LeaveCriticalSection(&state_lock);
}
static void observe_network(const void *bytes, size_t length) {
    if (!InterlockedCompareExchange(&ready,0,0) || !bytes || !length) return;
    InterlockedIncrement(&network_calls);
    EnterCriticalSection(&state_lock);
    cab_network(&state,(const uint8_t *)bytes,length,now_ms());
    log_transition();
    LeaveCriticalSection(&state_lock);
}
static BOOL relay_write(unsigned slot,HANDLE handle,LPCVOID bytes,DWORD length,LPDWORD written,LPOVERLAPPED overlap) {
    BOOL result=((write_proc)modules[slot].original[0])(handle,bytes,length,written,overlap);
    DWORD error=GetLastError();
    if (result) observe_write(handle,bytes,written ? *written : length);
    else if (overlap && error == ERROR_IO_PENDING) observe_write(handle,bytes,length);
    SetLastError(error);
    return result;
}
static int relay_send(unsigned slot,SOCKET socket_value,const char *bytes,int length,int flags) {
    int result=((send_proc)modules[slot].original[1])(socket_value,bytes,length,flags);
    int error=WSAGetLastError();
    if (result>0) observe_network(bytes,(size_t)result);
    WSASetLastError(error);
    return result;
}
static int relay_wsasend(unsigned slot,SOCKET socket_value,LPWSABUF buffers,DWORD count,LPDWORD sent,DWORD flags,LPWSAOVERLAPPED overlap,LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    int result=((wsasend_proc)modules[slot].original[2])(socket_value,buffers,count,sent,flags,overlap,completion);
    int error=WSAGetLastError();
    DWORD index,remaining=(result==0 && sent) ? *sent : 0;
    if (result==SOCKET_ERROR && overlap && error==WSA_IO_PENDING) remaining=0xFFFFFFFF;
    for (index=0; index<count && remaining; index++) {
        DWORD length=buffers[index].len < remaining ? buffers[index].len : remaining;
        observe_network(buffers[index].buf,length);
        remaining-=length;
    }
    WSASetLastError(error);
    return result;
}
static HANDLE relay_request(unsigned slot,HANDLE connection,LPCWSTR verb,LPCWSTR object,LPCWSTR version,LPCWSTR referrer,LPCWSTR *accept_types,DWORD flags) {
    HANDLE result=((request_proc)modules[slot].original[3])(connection,verb,object,version,referrer,accept_types,flags);
    DWORD error=GetLastError();
    char path[1024];
    if (result && object && WideCharToMultiByte(CP_UTF8,0,object,-1,path,sizeof(path),NULL,NULL)>0) observe_network(path,strlen(path));
    SetLastError(error);
    return result;
}
#define WRAPPERS(slot) \
static BOOL WINAPI hooked_write_##slot(HANDLE handle,LPCVOID bytes,DWORD length,LPDWORD written,LPOVERLAPPED overlap) { return relay_write(slot,handle,bytes,length,written,overlap); } \
static int WSAAPI hooked_send_##slot(SOCKET socket_value,const char *bytes,int length,int flags) { return relay_send(slot,socket_value,bytes,length,flags); } \
static int WSAAPI hooked_wsasend_##slot(SOCKET socket_value,LPWSABUF buffers,DWORD count,LPDWORD sent,DWORD flags,LPWSAOVERLAPPED overlap,LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) { return relay_wsasend(slot,socket_value,buffers,count,sent,flags,overlap,completion); } \
static HANDLE WINAPI hooked_request_##slot(HANDLE connection,LPCWSTR verb,LPCWSTR object,LPCWSTR version,LPCWSTR referrer,LPCWSTR *accept_types,DWORD flags) { return relay_request(slot,connection,verb,object,version,referrer,accept_types,flags); }
WRAPPERS(0)
WRAPPERS(1)
WRAPPERS(2)
WRAPPERS(3)
WRAPPERS(4)
WRAPPERS(5)
WRAPPERS(6)
WRAPPERS(7)
WRAPPERS(8)
WRAPPERS(9)
WRAPPERS(10)
WRAPPERS(11)
WRAPPERS(12)
WRAPPERS(13)
WRAPPERS(14)
WRAPPERS(15)
WRAPPERS(16)
WRAPPERS(17)
WRAPPERS(18)
WRAPPERS(19)
WRAPPERS(20)
WRAPPERS(21)
WRAPPERS(22)
WRAPPERS(23)
WRAPPERS(24)
WRAPPERS(25)
WRAPPERS(26)
WRAPPERS(27)
WRAPPERS(28)
WRAPPERS(29)
WRAPPERS(30)
WRAPPERS(31)
#define WRAPPER_ROW(slot) { (PVOID)hooked_write_##slot,(PVOID)hooked_send_##slot,(PVOID)hooked_wsasend_##slot,(PVOID)hooked_request_##slot }
static PVOID replacements[MODULE_LIMIT][4] = { WRAPPER_ROW(0),WRAPPER_ROW(1),WRAPPER_ROW(2),WRAPPER_ROW(3),WRAPPER_ROW(4),WRAPPER_ROW(5),WRAPPER_ROW(6),WRAPPER_ROW(7),WRAPPER_ROW(8),WRAPPER_ROW(9),WRAPPER_ROW(10),WRAPPER_ROW(11),WRAPPER_ROW(12),WRAPPER_ROW(13),WRAPPER_ROW(14),WRAPPER_ROW(15),WRAPPER_ROW(16),WRAPPER_ROW(17),WRAPPER_ROW(18),WRAPPER_ROW(19),WRAPPER_ROW(20),WRAPPER_ROW(21),WRAPPER_ROW(22),WRAPPER_ROW(23),WRAPPER_ROW(24),WRAPPER_ROW(25),WRAPPER_ROW(26),WRAPPER_ROW(27),WRAPPER_ROW(28),WRAPPER_ROW(29),WRAPPER_ROW(30),WRAPPER_ROW(31) };
static int hook_kind(const char *library,const char *name) {
    if (!_stricmp(name,"WriteFile") && (!_stricmp(library,"KERNEL32.dll") || !_stricmp(library,"KERNELBASE.dll") || !_strnicmp(library,"api-ms-win-core-file-",21))) return 0;
    if (!_stricmp(library,"WS2_32.dll")) {
        if (!strcmp(name,"send")) return 1;
        if (!strcmp(name,"WSASend")) return 2;
    }
    if (!_stricmp(library,"WINHTTP.dll") && !strcmp(name,"WinHttpOpenRequest")) return 3;
    return -1;
}
static void patch_module(unsigned slot) {
    BYTE *base=(BYTE *)modules[slot].module;
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *descriptor;
    DWORD directory;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt=(IMAGE_NT_HEADERS *)(base+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!directory) return;
    descriptor=(IMAGE_IMPORT_DESCRIPTOR *)(base+directory);
    for (; descriptor->Name; descriptor++) {
        IMAGE_THUNK_DATA *names,*targets;
        char *library=(char *)(base+descriptor->Name);
        if (!descriptor->OriginalFirstThunk) continue;
        names=(IMAGE_THUNK_DATA *)(base+descriptor->OriginalFirstThunk);
        targets=(IMAGE_THUNK_DATA *)(base+descriptor->FirstThunk);
        for (; names->u1.AddressOfData; names++,targets++) {
            const char *name;
            int kind;
            DWORD protection,ignored;
            PVOID original;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                unsigned ordinal=(unsigned)IMAGE_ORDINAL(names->u1.Ordinal);
                if (_stricmp(library,"WS2_32.dll") || ordinal != 19) continue;
                name="send";
            } else name=(char *)((IMAGE_IMPORT_BY_NAME *)(base+names->u1.AddressOfData))->Name;
            kind=hook_kind(library,name);
            if (kind<0 || (modules[slot].mask & (1u<<kind))) continue;
            original=(PVOID)(ULONG_PTR)targets->u1.Function;
            if (!original || original==replacements[slot][kind]) continue;
            if (!VirtualProtect(&targets->u1.Function,sizeof(PVOID),PAGE_READWRITE,&protection)) continue;
            modules[slot].original[kind]=original;
            if (InterlockedCompareExchangePointer((PVOID volatile *)&targets->u1.Function,replacements[slot][kind],original)==original) {
                modules[slot].mask|=1u<<kind;
                InterlockedIncrement(&hook_count);
                log_line("hook module_slot=%u import=%s",slot,name);
            }
            VirtualProtect(&targets->u1.Function,sizeof(PVOID),protection,&ignored);
        }
    }
}
static void scan_modules(void) {
    HANDLE snapshot;
    MODULEENTRY32W entry;
    wchar_t system_root[MAX_PATH];
    unsigned slot;
    GetWindowsDirectoryW(system_root,MAX_PATH);
    snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if (snapshot==INVALID_HANDLE_VALUE) return;
    memset(&entry,0,sizeof(entry)); entry.dwSize=sizeof(entry);
    if (Module32FirstW(snapshot,&entry)) do {
        HMODULE pinned;
        size_t root_length=wcslen(system_root);
        if (entry.hModule==self_module || (!_wcsnicmp(entry.szExePath,system_root,root_length) && entry.szExePath[root_length]==L'\\')) continue;
        for (slot=0;slot<MODULE_LIMIT;slot++) if (!modules[slot].module || modules[slot].module==entry.hModule) break;
        if (slot==MODULE_LIMIT) continue;
        if (!modules[slot].module) {
            char module_name[MAX_PATH*3];
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)entry.modBaseAddr,&pinned)) continue;
            modules[slot].module=entry.hModule;
            if (WideCharToMultiByte(CP_UTF8,0,entry.szModule,-1,module_name,sizeof(module_name),NULL,NULL)) log_line("module_slot=%u name=%s",slot,module_name);
        }
        patch_module(slot);
    } while (Module32NextW(snapshot,&entry));
    CloseHandle(snapshot);
}
static unsigned read_number(const wchar_t *section,const wchar_t *key,unsigned fallback,unsigned minimum,unsigned maximum) {
    unsigned value=GetPrivateProfileIntW(section,key,fallback,ini_path);
    if (value<minimum || value>maximum) {
        log_line("invalid numeric configuration; default=%u",fallback);
        return fallback;
    }
    return value;
}
static int send_all(SOCKET client,const char *bytes,int length) {
    int offset=0;
    while (offset<length) {
        int sent=send(client,bytes+offset,length-offset,0);
        if (sent<=0) return 0;
        offset+=sent;
    }
    return 1;
}
static void serve_client(SOCKET client) {
    char request[2048],method[16],path[256],body[1600],header[512];
    int received,total=0,status=200,is_head=0;
    const char *phrase="OK";
    DWORD timeout=1000;
    cab_state snapshot;
    uint64_t age;
    setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof(timeout));
    setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,(const char *)&timeout,sizeof(timeout));
    while (total<(int)sizeof(request)-1) {
        received=recv(client,request+total,sizeof(request)-1-total,0);
        if (received<=0) return;
        total+=received; request[total]=0;
        if (strstr(request,"\r\n\r\n")) break;
    }
    if (sscanf(request,"%15s %255s",method,path)!=2) return;
    is_head=!strcmp(method,"HEAD");
    if (strchr(path,'?')) *strchr(path,'?')=0;
    EnterCriticalSection(&state_lock);
    snapshot=state;
    LeaveCriticalSection(&state_lock);
    age=now_ms()-snapshot.last_signal_ms;
    if (strcmp(method,"GET") && !is_head) {
        status=405; phrase="Method Not Allowed"; strcpy(body,"{\"error\":\"method_not_allowed\"}");
    } else if (!strcmp(path,"/debug")) {
        snprintf(body,sizeof(body),"{\"version\":\"%s\",\"pid\":%lu,\"state\":\"%s\",\"reason\":\"%s\",\"revision\":%lu,\"polls\":%lu,\"valid_frames\":%lu,\"invalid_frames\":%lu,\"write_calls\":%ld,\"network_calls\":%ld,\"hooks\":%ld,\"last_signal_age_ms\":%lu,\"radio_on_requests\":%lu,\"radio_off_requests\":%lu,\"poll_streak\":%lu,\"last_command\":%u,\"occupied_timeout_seconds\":%lu}",
          CABINET_VERSION,(unsigned long)GetCurrentProcessId(),cab_phase_name(snapshot.phase),snapshot.reason,(unsigned long)snapshot.revision,(unsigned long)snapshot.polls,(unsigned long)snapshot.valid_frames,(unsigned long)snapshot.invalid_frames,
          InterlockedCompareExchange(&write_calls,0,0),InterlockedCompareExchange(&network_calls,0,0),InterlockedCompareExchange(&hook_count,0,0),(unsigned long)age,
          (unsigned long)snapshot.radio_on_requests,(unsigned long)snapshot.radio_off_requests,(unsigned long)snapshot.poll_streak,(unsigned)snapshot.last_command,(unsigned long)config.idle_seconds);
    } else if (!strcmp(path,"/status")) {
        if (snapshot.phase==CAB_UNKNOWN) {
            status=503; phrase="Service Unavailable"; strcpy(body,"{\"error\":\"state_unavailable\",\"state\":\"unknown\"}");
        } else snprintf(body,sizeof(body),"{\"status\":\"%s\"}",cab_phase_name(snapshot.phase));
    } else {
        status=404; phrase="Not Found"; strcpy(body,"{\"error\":\"not_found\"}");
    }
    snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n%s\r\n",status,phrase,(unsigned)strlen(body),status==405 ? "Allow: GET, HEAD\r\n" : "");
    if (send_all(client,header,(int)strlen(header)) && !is_head) send_all(client,body,(int)strlen(body));
}
static DWORD WINAPI worker(LPVOID unused) {
    HMODULE pinned;
    wchar_t *slash;
    char address[64];
    WSADATA winsock;
    SOCKET listener=INVALID_SOCKET;
    struct sockaddr_in endpoint;
    unsigned port,enabled;
    DWORD path_length;
    uint64_t last_scan=0,last_report;
    int exclusive=1,error;
    (void)unused;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)self_module,&pinned);
    QueryPerformanceFrequency(&clock_frequency);
    InitializeCriticalSection(&state_lock);
    path_length=GetModuleFileNameW(self_module,ini_path,MAX_PATH);
    if (!path_length || path_length>=MAX_PATH || !(slash=wcsrchr(ini_path,L'\\'))) return 1;
    slash[1]=0;
    wcscpy(log_path,ini_path);
    if (wcslen(ini_path)+24>=MAX_PATH) return 1;
    wcscat(ini_path,L"cabinet-status.ini");
    wcscat(log_path,L"cabinet-status.log");
    log_file=CreateFileW(log_path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (log_file==INVALID_HANDLE_VALUE) {
        DWORD log_error=GetLastError();
        DWORD temp_length=GetTempPathW(MAX_PATH,log_path);
        if (temp_length && temp_length<MAX_PATH-40) {
            wcscat(log_path,L"cabinet-status-fallback.log");
            log_file=CreateFileW(log_path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        }
        log_line("Package log unavailable error=%lu; using TEMP fallback",(unsigned long)log_error);
    }
    log_line("starting version=%s bits=%u pid=%lu",CABINET_VERSION,(unsigned)(sizeof(void *)*8),(unsigned long)GetCurrentProcessId());
    cab_defaults(&config);
    config.hold_seconds=read_number(L"Cabinet",L"MinOccupiedHoldSeconds",45,0,3600);
    config.guest_seconds=read_number(L"Cabinet",L"GuestRadioOffSeconds",6,0,300);
    config.idle_seconds=read_number(L"Cabinet",L"IdleTimeoutSeconds",0,0,86400);
    config.signal_seconds=read_number(L"Cabinet",L"SignalTimeoutSeconds",60,1,3600);
    config.reader_address=(uint8_t)read_number(L"Cabinet",L"ReaderAddress",0,0,255);
    port=read_number(L"Server",L"Port",18080,1,65535);
    enabled=read_number(L"Server",L"Enabled",1,0,1);
    log_line("timers hold=%u guest=%u occupied_expiry=%u idle_expiry=%u reader=%u",config.hold_seconds,config.guest_seconds,config.idle_seconds,config.signal_seconds,config.reader_address);
    {
        wchar_t wide_address[64];
        GetPrivateProfileStringW(L"Server",L"BindAddress",L"0.0.0.0",wide_address,64,ini_path);
        if (!WideCharToMultiByte(CP_ACP,0,wide_address,-1,address,sizeof(address),NULL,NULL)) strcpy(address,"0.0.0.0");
    }
    cab_init(&state,now_ms());
    last_report=now_ms();
    error=WSAStartup(MAKEWORD(2,2),&winsock);
    if (error) { log_line("WSAStartup failed error=%d",error); return 1; }
    InterlockedExchange(&ready,1);
    scan_modules();
    if (enabled) {
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        memset(&endpoint,0,sizeof(endpoint));
        endpoint.sin_family=AF_INET; endpoint.sin_port=htons((u_short)port); endpoint.sin_addr.s_addr=inet_addr(address);
        setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char *)&exclusive,sizeof(exclusive));
        if (listener==INVALID_SOCKET || endpoint.sin_addr.s_addr==INADDR_NONE || bind(listener,(struct sockaddr *)&endpoint,sizeof(endpoint)) || listen(listener,4)) {
            log_line("HTTP listen failed address=%s port=%u WSA=%d",address,port,WSAGetLastError());
            if (listener!=INVALID_SOCKET) closesocket(listener);
            listener=INVALID_SOCKET;
        } else log_line("HTTP listening address=%s port=%u",address,port);
    }
    for (;;) {
        uint64_t timestamp=now_ms();
        EnterCriticalSection(&state_lock);
        cab_tick(&state,&config,timestamp);
        log_transition();
        if (timestamp-last_report>=30000) {
            log_line("diagnostics pid=%lu state=%s polls=%lu streak=%lu radio_on=%lu radio_off=%lu valid=%lu invalid=%lu last_cmd=0x%02X",(unsigned long)GetCurrentProcessId(),cab_phase_name(state.phase),
                     (unsigned long)state.polls,(unsigned long)state.poll_streak,(unsigned long)state.radio_on_requests,(unsigned long)state.radio_off_requests,
                     (unsigned long)state.valid_frames,(unsigned long)state.invalid_frames,(unsigned)state.last_command);
            last_report=timestamp;
        }
        LeaveCriticalSection(&state_lock);
        if (timestamp-last_scan>=3000) { scan_modules(); last_scan=timestamp; }
        if (listener!=INVALID_SOCKET) {
            fd_set readable;
            struct timeval timeout;
            FD_ZERO(&readable); FD_SET(listener,&readable);
            timeout.tv_sec=0; timeout.tv_usec=200000;
            if (select(0,&readable,NULL,NULL,&timeout)>0) {
                SOCKET client=accept(listener,NULL,NULL);
                if (client!=INVALID_SOCKET) { serve_client(client); closesocket(client); }
            }
        } else Sleep(200);
    }
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)reserved;
    if (reason==DLL_PROCESS_ATTACH) {
        HANDLE thread;
        self_module=instance;
        DisableThreadLibraryCalls(instance);
        thread=CreateThread(NULL,0,worker,NULL,0,NULL);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}

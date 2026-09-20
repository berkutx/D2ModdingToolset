// Execute actual production shims and Detours transaction against synthetic
// code in this process. No game process/module or network is accessed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../features/netboundarytrace.cpp"

struct Event { unsigned code; uintptr_t obj, a, b, c, d; };
static std::vector<Event> events;
static bool enabled = true;
extern "C" int c4trace_enabled() { return enabled; }
extern "C" int inventorytrace_exact_exe() { return 0; }
extern "C" void c4trace_event(unsigned code, uintptr_t obj, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    events.push_back({code,obj,a,b,c,d});
    SetLastError(0xBAD0); // Observer must not leak logger side effects.
}
static unsigned failures, cases, nativeCalls, targetId;
static uintptr_t entry, heldVtable, selfAddress, arg1, arg2, expectedEsp;
static uintptr_t observedEsp, observedEbp, observedEbx, observedEsi, observedEdi;
static uint32_t observedResult, observedFlags, observedLocal, expectedEbp;
static unsigned kind, mode;
static int nativeResult;
static constexpr DWORD EntryError = 0xCA11, NativeError = 0xCA22;
static unsigned char selectedFrame[57];
static uint32_t sender;
static const char* caseName = "setup";
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %s:%d %s\n",caseName,__LINE__,#x); } } while(0)

void checkTarget(void* self, unsigned id)
{
    ++nativeCalls; targetId = id;
    CHECK(reinterpret_cast<uintptr_t>(self) == selfAddress);
    CHECK(GetLastError() == EntryError);
    SetLastError(NativeError);
    if (mode == 1) RaiseException(0xE0424242, 0, 0, nullptr);
}
bool __fastcall sendA(void* self, void*, uint32_t to, const void* message)
{ checkTarget(self,1); CHECK(to==arg1); CHECK(reinterpret_cast<uintptr_t>(message)==arg2); return nativeResult!=0; }
bool __fastcall sendB(void* self, void*, uint32_t to, const void* message)
{ checkTarget(self,2); CHECK(to==arg1); CHECK(reinterpret_cast<uintptr_t>(message)==arg2); return nativeResult!=0; }
int __fastcall recvA(void* self, void*, uint32_t* from, void* message)
{ checkTarget(self,1); CHECK(reinterpret_cast<uintptr_t>(from)==arg1); CHECK(reinterpret_cast<uintptr_t>(message)==arg2); return nativeResult; }
int __fastcall recvB(void* self, void*, uint32_t* from, void* message)
{ checkTarget(self,2); CHECK(reinterpret_cast<uintptr_t>(from)==arg1); CHECK(reinterpret_cast<uintptr_t>(message)==arg2); return nativeResult; }
int __fastcall countA(void* self, void*) { checkTarget(self,1); return nativeResult; }
int __fastcall countB(void* self, void*) { checkTarget(self,2); return nativeResult; }

__declspec(naked) void finish()
{
    __asm {
        mov observedEsp, esp
        mov observedEbp, ebp
        mov observedEbx, ebx
        mov observedEsi, esi
        mov observedEdi, edi
        mov observedResult, eax
        pushfd
        pop observedFlags
        mov eax, [ebp-4]
        mov observedLocal, eax
        mov esp, expectedEsp
        add esp, 16
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

__declspec(naked) void invoke()
{
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        sub esp, 16
        mov expectedEsp, esp
        lea ebp, [esp+8]
        mov expectedEbp, ebp
        mov dword ptr [ebp-4], 0AABBCCDDh
        cmp kind, 2
        je noargs
        push arg2
        push arg1
    noargs:
        mov ecx, selfAddress
        mov eax, heldVtable
        mov edx, heldVtable
        mov ebx, 013579BDFh
        mov esi, 02468ACE0h
        mov edi, 0FEDCBA98h
        jmp dword ptr [entry]
    }
}

// Restore native caller's SEH chain normally after exceptions escape our shim.
bool invokeCaught()
{
    __try { invoke(); return false; }
    __except (GetExceptionCode()==0xE0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
}

static void initializeFrame()
{
    std::memset(selectedFrame,0,sizeof(selectedFrame));
    uint32_t type=0xFFFF, length=sizeof(selectedFrame);
    std::memcpy(selectedFrame,&type,4); std::memcpy(selectedFrame+4,&length,4);
    const char selectedClass[]=".?AVCCmdEndTurnMsg@@";
    std::memcpy(selectedFrame+8,selectedClass,sizeof(selectedClass));
    for (unsigned i=44;i<sizeof(selectedFrame);++i) selectedFrame[i]=static_cast<unsigned char>(i);
}

int main()
{
    static_assert(sizeof(void*)==4,"x86 ABI fixture only");
    initializeFrame();
    events.reserve(4096);
    void* tableA[9]={}, *tableB[9]={};
    tableA[4]=reinterpret_cast<void*>(countA); tableA[5]=reinterpret_cast<void*>(sendA); tableA[6]=reinterpret_cast<void*>(recvA);
    tableB[4]=reinterpret_cast<void*>(countB); tableB[5]=reinterpret_cast<void*>(sendB); tableB[6]=reinterpret_cast<void*>(recvB);
    void** selfTable=tableA; selfAddress=reinterpret_cast<uintptr_t>(&selfTable);
    heldVtable=reinterpret_cast<uintptr_t>(tableB); // Deliberately differs from object's table.
    BYTE* code=static_cast<BYTE*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    CHECK(code!=nullptr); if(!code) return 1;
    const unsigned char* signatures[]={kClientSend,kServerSend,kClientReceive,kServerReceive,kClientCount,kServerCount};
    const size_t sizes[]={sizeof(kClientSend),sizeof(kServerSend),sizeof(kClientReceive),sizeof(kServerReceive),sizeof(kClientCount),sizeof(kServerCount)};
    void* shims[]={clientSendShim,serverSendShim,clientReceiveShim,serverReceiveShim,clientCountShim,serverCountShim};
    uintptr_t* returns[]={&g_clientSendReturn,&g_serverSendReturn,&g_clientReceiveReturn,&g_serverReceiveReturn,&g_clientCountReturn,&g_serverCountReturn};
    Site sites[6]={};
    for(unsigned i=0;i<6;++i) {
        BYTE* site=code+i*128;
        std::memcpy(site,signatures[i],sizes[i]);
        site[sizes[i]]=0xE9;
        const int32_t delta=static_cast<int32_t>(reinterpret_cast<uintptr_t>(finish)-reinterpret_cast<uintptr_t>(site+sizes[i]+5));
        std::memcpy(site+sizes[i]+1,&delta,4);
        *returns[i]=reinterpret_cast<uintptr_t>(site+sizes[i]);
        sites[i]={reinterpret_cast<uintptr_t>(site),signatures[i],sizes[i],shims[i],1u<<i,nullptr};
    }
    FlushInstructionCache(GetCurrentProcess(),code,4096);

    // Compare original virtual-call windows and patched production shims. The
    // held register intentionally points to a different vtable from *self.
    for(unsigned patched=0;patched<2;++patched) {
        if(patched) { installSites(sites,6); CHECK(g_installedMask==63); }
        for(unsigned trace=0;trace<2;++trace) for(unsigned site=0;site<6;++site) for(unsigned value=0;value<3;++value) {
            char name[100]; sprintf_s(name,"patched=%u trace=%u site=%u value=%u",patched,trace,site,value); caseName=name; ++cases;
            enabled=trace!=0; mode=0; nativeCalls=targetId=0; events.clear();
            kind=site/2; entry=sites[site].address;
            nativeResult=kind==0 ? (value!=0) : kind==1 ? (value==0 ? 0 : value==1 ? 2 : 3) : (value==0 ? 0 : value==1 ? 7 : -1);
            sender=0x12345678; arg1=kind==0 ? 0x778899 : reinterpret_cast<uintptr_t>(&sender); arg2=reinterpret_cast<uintptr_t>(selectedFrame);
            SetLastError(EntryError); invoke(); const DWORD resultError=GetLastError();
            CHECK(nativeCalls==1); CHECK(targetId==((site==0||site==4)?1u:2u));
            CHECK(resultError==NativeError); CHECK(observedEsp==expectedEsp); CHECK(observedEbp==expectedEbp);
            CHECK(observedEbx==0x13579BDF); CHECK(observedEsi==0x2468ACE0); CHECK(observedEdi==0xFEDCBA98);
            if(kind==0) CHECK((observedResult&255u)==uint32_t(nativeResult!=0)); else CHECK(observedResult==uint32_t(nativeResult));
            if(site==1) CHECK(observedResult==uint32_t(nativeResult!=0));
            if(site==2) CHECK(((observedFlags>>6)&1u)==uint32_t(nativeResult==3));
            if(site==3) CHECK(observedLocal==uint32_t(nativeResult)); else CHECK(observedLocal==0xAABBCCDD);
            if(site==5) CHECK(((observedFlags>>6)&1u)==uint32_t(nativeResult==0));
            if(patched&&trace&&kind==0) {
                CHECK(events.size()==4); CHECK(events.front().code==C4NET_SEND_ENTER); CHECK(events.back().code==C4NET_SEND_RESULT);
                CHECK(events.front().a==events.back().a); CHECK(events.back().c==uint32_t(nativeResult!=0));
            } else if(patched&&trace&&kind==1&&nativeResult==2) {
                CHECK(events.size()==3); CHECK(events[0].code==C4NET_RECEIVE); CHECK(events[1].d==1);
            } else CHECK(events.empty());
        }
    }
    // Native faults propagate to the caller; logger/counters finish unwinding.
    enabled=true;
    for(unsigned site=0;site<6;++site) {
        caseName="exception propagation"; ++cases; events.clear(); mode=1; kind=site/2; entry=sites[site].address;
        arg1=kind==0 ? 0x778899 : reinterpret_cast<uintptr_t>(&sender); arg2=reinterpret_cast<uintptr_t>(selectedFrame);
        SetLastError(EntryError); CHECK(invokeCaught()); CHECK(GetLastError()==NativeError);
        CHECK(g_counters[0].activeCalls==0&&g_counters[1].activeCalls==0);
        CHECK(!events.empty()&&events.back().code==C4NET_EXCEPTION);
    }
    mode=0;
    caseName="unreadable successful sender is explicit"; ++cases; events.clear(); kind=1; entry=sites[2].address; nativeResult=2;
    arg1=1; arg2=reinterpret_cast<uintptr_t>(selectedFrame); SetLastError(EntryError); invoke();
    CHECK(events.size()==3&&events[1].d==2); CHECK(GetLastError()==NativeError);

    caseName="empty/failed receive never inspects payload"; ++cases; events.clear();
    arg1=arg2=1; nativeResult=0; SetLastError(EntryError); invoke(); CHECK(events.empty()); CHECK(GetLastError()==NativeError);
    nativeResult=3; SetLastError(EntryError); invoke(); CHECK(events.empty()); CHECK(GetLastError()==NativeError);

    caseName="unreadable send passes through"; ++cases; events.clear(); kind=0; entry=sites[0].address;
    arg1=0x778899; arg2=1; nativeResult=1; SetLastError(EntryError); invoke(); CHECK(events.empty()); CHECK(GetLastError()==NativeError);

    caseName="passive sample and off mode"; ++cases; events.clear(); const unsigned before=nativeCalls;
    SetLastError(EntryError); netboundarytrace_sample(); CHECK(GetLastError()==EntryError); CHECK(events.size()==6); CHECK(nativeCalls==before);
    C4NetBoundaryCounters sample={}; netboundarytrace_sampleCounters(&sample); CHECK(sample.installedMask==63);
    CHECK(sample.client.activeCalls==0&&sample.server.activeCalls==0); CHECK(sample.client.exceptions==3&&sample.server.exceptions==3);
    enabled=false; events.clear(); netboundarytrace_sample(); CHECK(events.empty());

    caseName="unsupported executable is not patched"; ++cases; enabled=true; events.clear();
    netboundarytrace_install(); CHECK(events.size()==1&&events[0].code==C4NET_BOUNDARY_UNAVAILABLE&&events[0].a==1);
    caseName="modified site rejected"; ++cases; events.clear(); Site mismatch=sites[0]; mismatch.address=reinterpret_cast<uintptr_t>(code+1024);
    std::memset(code+1024,0x90,16); installSites(&mismatch,1); CHECK(events.size()==1&&events[0].a==2); CHECK(code[1024]==0x90);
    std::printf("netboundarytrace ABI: %u cases, %u failures\n",cases,failures);
    return failures?1:0;
}

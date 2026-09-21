// Production WakeState + current MSS methods, real worker/UI threads and Win32 messages.
// FIFO/packet callbacks and game accessors are substitutes, not real gameplay.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "../features/netnotifystate.h"

static std::atomic<unsigned> failures{0};
static unsigned cases;
static const char* caseName;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %s line %u: %s\n", caseName, __LINE__, #x); } } while (0)
static void beginCase(const char* name) { caseName=name; ++cases; }
static void endCase(unsigned before) { std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", caseName); }
enum DefaultMessageIDTypes { ID_TURN=1, ID_DISCONNECT=2, ID_OTHER=3 };
struct Packet { unsigned char data[1]; unsigned sequence; };
struct RakPeerInterface { virtual ~RakPeerInterface() = default; };
struct Fixture;
static Fixture* current;
namespace spdlog { template<class... Args> void debug(const char*, Args...) {} }
namespace game {
struct UIManagerPtr { void* data; };
using SmartPointer=UIManagerPtr;
struct BaseService { void* vftable; };
struct MidgardData { BaseService* netService=nullptr; };
struct Midgard { MidgardData* data; };
static MidgardData worldData;
static Midgard world{&worldData};
struct CMidgardApi {
    struct Api { Midgard* (*instance)(); };
    static const Api& get() { static const Api api{[](){return &world;}}; return api; }
};
static void getManager(UIManagerPtr*);
static bool postMessage(const void*,unsigned,unsigned,long);
struct CUIManagerApi {
    struct Api { void (*get)(UIManagerPtr*); bool (*postMessage)(const void*,unsigned,unsigned,long); };
    static const Api& get() { static const Api api{getManager,postMessage}; return api; }
};
static void releaseManager(SmartPointer*,void*);
struct SmartPointerApi {
    struct Api { void (*createOrFree)(SmartPointer*,void*); };
    static const Api& get() { static const Api api{releaseManager}; return api; }
};
}
namespace hooks {
static int g_vftable;
struct CNetCustomPeer : RakPeerInterface {
    std::atomic<bool> m_packetNotificationSent{false};
    unsigned m_packetNotificationMessageId;
    struct Mutex { std::mutex value; void Lock(){value.lock();} void Unlock(){value.unlock();} } packetReturnMutex;
    struct Queue { std::deque<Packet*> packets; bool IsEmpty() const{return packets.empty();} } packetReturnQueue;
    unsigned receiveCalls=0,deallocated=0;
    explicit CNetCustomPeer(unsigned id):m_packetNotificationMessageId(id){}
    ~CNetCustomPeer(){for(auto p:packetReturnQueue.packets)delete p;}
    bool IsPacketNotificationSent() const;
    void ResetPacketNotification();
    static void UpdateThreadCallback(RakPeerInterface*,void*);
    void SendPacketNotification();
    Packet* Receive(){
        std::lock_guard<std::mutex> lock(packetReturnMutex.value); ++receiveCalls;
        if(packetReturnQueue.IsEmpty())return nullptr;
        auto p=packetReturnQueue.packets.front();packetReturnQueue.packets.pop_front();return p;
    }
    void DeallocatePacket(Packet* p){++deallocated;delete p;}
    void push(DefaultMessageIDTypes type,unsigned sequence){
        std::lock_guard<std::mutex> lock(packetReturnMutex.value);
        packetReturnQueue.packets.push_back(new Packet{{static_cast<unsigned char>(type)},sequence});
    }
    size_t count(){std::lock_guard<std::mutex> lock(packetReturnMutex.value);return packetReturnQueue.packets.size();}
};
struct NetPeerCallback {
    std::vector<unsigned> sequences;
    HANDLE entered=nullptr, allowReturn=nullptr;
    void onPacketReceived(DefaultMessageIDTypes,CNetCustomPeer*,Packet* p){
        sequences.push_back(p->sequence);
        if(entered){CHECK(SetEvent(entered));CHECK(WaitForSingleObject(allowReturn,5000)==WAIT_OBJECT_0);}
    }
};
struct CNetCustomService:game::BaseService {
    CNetCustomPeer* m_peer;
    NetPeerCallback recorder;
    explicit CNetCustomService(CNetCustomPeer* peer):m_peer(peer){vftable=&g_vftable;}
    static CNetCustomService* get();
    static void __fastcall peerProcessEventCallback(const CNetCustomService*,int,unsigned int,long);
    std::vector<NetPeerCallback*> getPeerCallbacks(){return {&recorder};}
};
#include "netnotifystate-peer.generated.inc"
#include "netnotifystate-service.generated.inc"
}
using Wake=c4net::WakeState;
struct Fixture {
    HWND window=nullptr;
    UINT message=0;
    DWORD uiTid=GetCurrentThreadId();
    std::atomic<DWORD> workerTid{0};
    HANDLE posted=CreateEventA(nullptr,TRUE,FALSE,nullptr);
    HANDLE allowReturn=CreateEventA(nullptr,TRUE,FALSE,nullptr);
    bool holdPostReturn=false;
    std::atomic<unsigned> posts{0},gets{0},releases{0},recoveryPosts{0};
    std::atomic<unsigned> windowWakes{0};
    unsigned uiBeats=0,translated=0,deferred=0,stale=0;
    std::mutex wakeMutex;
    Wake wake;
    std::unique_ptr<hooks::CNetCustomPeer> peer;
    std::unique_ptr<hooks::CNetCustomService> service;
    ~Fixture(){CloseHandle(posted);CloseHandle(allowReturn);}
    uintptr_t object() const{return reinterpret_cast<uintptr_t>(service.get());}
    uintptr_t data() const{return reinterpret_cast<uintptr_t>(peer.get());}
    void registerCurrent(){std::lock_guard<std::mutex> lock(wakeMutex);CHECK(wake.registered(object(),data(),0));}
    void cancelRegistration(){std::lock_guard<std::mutex> lock(wakeMutex);wake.cancel();}
    bool postTicket(Wake::Ticket ticket,HWND target){
        bool result=PostMessageA(target,WM_APP+2,ticket.sequence,static_cast<LPARAM>(ticket.generation))!=FALSE;
        if(result)++recoveryPosts;
        else {std::lock_guard<std::mutex> lock(wakeMutex);wake.postFailed(ticket);}
        return result;
    }
    bool requestPrivate(Wake::Ticket* result=nullptr){
        Wake::Ticket ticket{};
        {std::lock_guard<std::mutex> lock(wakeMutex);if(!wake.reserve(&ticket))return false;}
        if(result)*result=ticket;
        CHECK(postTicket(ticket,window));return true;
    }
    void requestWorker(unsigned count=1){std::thread worker([this,count](){for(unsigned i=0;i<count;++i)requestPrivate();});worker.join();}
    bool preparePrivate(MSG& msg){
        if(msg.message!=WM_APP+2)return true;
        const Wake::Ticket ticket{static_cast<uint32_t>(msg.lParam),static_cast<uint32_t>(msg.wParam)};
        std::lock_guard<std::mutex> lock(wakeMutex);
        if(!wake.consume(ticket)){++stale;return false;}
        ++translated;msg.message=message;msg.wParam=0;msg.lParam=0;return true;
    }
    void requeueDeferred(){
        Wake::Ticket ticket{};
        {std::lock_guard<std::mutex> lock(wakeMutex);if(!wake.requeue(&ticket))return;}
        CHECK(postTicket(ticket,window));
    }
    void pump(bool atOuterBoundary=true){
        CHECK(GetCurrentThreadId()==uiTid);
        if(atOuterBoundary)requeueDeferred();
        MSG msg{};unsigned n=0;
        while(PeekMessageA(&msg,window,0,0,PM_REMOVE)){
            CHECK(++n<256);if(n>=256)break;
            // Classification is an explicit fixture input. Native hooks have separate tests.
            if(atOuterBoundary&&!preparePrivate(msg))continue;
            DispatchMessageA(&msg);
        }
    }
    void beat(){CHECK(PostMessageA(window,WM_APP+1,0,0));pump();}
    void tick(){hooks::CNetCustomPeer::UpdateThreadCallback(peer.get(),nullptr);}
    std::thread startHeldTick(){
        CHECK(ResetEvent(posted));CHECK(ResetEvent(allowReturn));holdPostReturn=true;
        return std::thread([this](){workerTid=GetCurrentThreadId();CHECK(workerTid!=uiTid);tick();});
    }
    void waitPosted(){CHECK(WaitForSingleObject(posted,5000)==WAIT_OBJECT_0);}
    void release(std::thread& thread){CHECK(SetEvent(allowReturn));thread.join();holdPostReturn=false;}
    void workerTicks(unsigned count){
        std::thread thread([this,count](){workerTid=GetCurrentThreadId();CHECK(workerTid!=uiTid);for(unsigned i=0;i<count;++i)tick();});thread.join();
    }
    void prime(){peer->push(ID_OTHER,0);workerTicks(1);pump();CHECK(wake.armed);}
};
namespace game {
void getManager(UIManagerPtr* v){++current->gets;v->data=current;}
bool postMessage(const void* value,unsigned msg,unsigned wp,long lp){
    CHECK(GetCurrentThreadId()==current->workerTid);
    CHECK(value==current&&msg==current->message&&wp==0&&lp==0);
    ++current->posts;
    bool success=PostMessageA(current->window,msg,wp,lp)!=FALSE;
    if(success&&current->holdPostReturn){
        CHECK(SetEvent(current->posted));
        // Actual PostMessage has returned; unchanged MSS has not assigned its atomic flag yet.
        CHECK(WaitForSingleObject(current->allowReturn,5000)==WAIT_OBJECT_0);
    }
    return success;
}
void releaseManager(SmartPointer* v,void*){CHECK(v->data==current);++current->releases;v->data=nullptr;}
}
static LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(current&&msg==current->message){
        CHECK(GetCurrentThreadId()==current->uiTid&&GetCurrentThreadId()!=current->workerTid);
        CHECK(hwnd==current->window&&wp==0&&lp==0);++current->windowWakes;
        uint32_t generation;
        {std::lock_guard<std::mutex> lock(current->wakeMutex);generation=current->wake.generation;}
        hooks::CNetCustomService::peerProcessEventCallback(current->service.get(),0,static_cast<unsigned>(wp),static_cast<long>(lp));
        {std::lock_guard<std::mutex> lock(current->wakeMutex);current->wake.completed(current->object(),current->data(),generation);}
        return 0;
    }
    if(current&&msg==WM_APP+2){
        const Wake::Ticket ticket{static_cast<uint32_t>(lp),static_cast<uint32_t>(wp)};
        std::lock_guard<std::mutex> lock(current->wakeMutex);
        if(current->wake.owns(ticket)){current->wake.defer(ticket);++current->deferred;}
        else ++current->stale;
        return 0;
    }
    if(current&&msg==WM_APP+1){++current->uiBeats;return 0;}
    return DefWindowProcA(hwnd,msg,wp,lp);
}
template<class Test> static void run(const char* name,Test test){
    unsigned before=failures;beginCase(name);Fixture f;current=&f;
    f.message=RegisterWindowMessageA("C4_PRIVATE_NETNOTIFYSTATE_TEST");
    f.window=CreateWindowExA(0,"C4NetNotifyStateTest","",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleA(nullptr),nullptr);
    if(!f.message||!f.window||!f.posted||!f.allowReturn){CHECK(false);current=nullptr;return;}
    f.peer=std::make_unique<hooks::CNetCustomPeer>(f.message);
    f.service=std::make_unique<hooks::CNetCustomService>(f.peer.get());game::worldData.netService=f.service.get();
    f.registerCurrent();test(f);CHECK(f.gets==f.releases);
    game::worldData.netService=nullptr;DestroyWindow(f.window);current=nullptr;endCase(before);
}
int main(){
    WNDCLASSA wc{};wc.lpfnWndProc=windowProc;wc.hInstance=GetModuleHandleA(nullptr);wc.lpszClassName="C4NetNotifyStateTest";
    if(!RegisterClassA(&wc))return 2;
    run("registered constructor stays unarmed until genuine callback in any context",[](Fixture& f){
        f.requestWorker(8);f.pump();CHECK(f.recoveryPosts==0&&f.windowWakes==0&&!f.wake.armed);
        f.peer->push(ID_OTHER,1);f.workerTicks(1);f.pump(false);CHECK(f.wake.armed);
        f.peer->push(ID_TURN,2);f.requestWorker();f.pump();
        CHECK((f.service->recorder.sequences==std::vector<unsigned>{1,2}));
    });
    run("MSS lost notification persists through 1024 producer ticks while UI answers",[](Fixture& f){
        f.peer->push(ID_OTHER,1);auto producer=f.startHeldTick();f.waitPosted();f.pump();
        CHECK(!f.peer->IsPacketNotificationSent());f.release(producer);CHECK(f.peer->IsPacketNotificationSent());
        f.peer->push(ID_TURN,2);f.peer->push(ID_DISCONNECT,3);
        for(unsigned i=0;i<32;++i){f.workerTicks(32);f.beat();}
        CHECK(f.posts==1&&f.windowWakes==1&&f.uiBeats==32&&f.peer->count()==2);
        f.requestWorker(32);CHECK(f.recoveryPosts==1);f.pump();
        CHECK((f.service->recorder.sequences==std::vector<unsigned>{1,2,3}));
        CHECK(f.translated==1&&!f.peer->IsPacketNotificationSent()&&f.peer->count()==0&&f.peer->deallocated==3);
        f.peer->push(ID_TURN,4);f.workerTicks(1);f.pump();
        CHECK((f.service->recorder.sequences==std::vector<unsigned>{1,2,3,4}));
    });
    run("renewable production tickets recover after one retry preceded the late store",[](Fixture& f){
        f.peer->push(ID_OTHER,1);auto producer=f.startHeldTick();f.waitPosted();f.pump();
        f.requestWorker();f.pump();CHECK(!f.peer->IsPacketNotificationSent());
        f.release(producer);f.peer->push(ID_DISCONNECT,2);f.workerTicks(1024);f.beat();
        CHECK(f.peer->count()==1&&f.windowWakes==2);
        f.requestWorker();f.pump();CHECK(f.peer->count()==0&&f.recoveryPosts==2);
        CHECK((f.service->recorder.sequences==std::vector<unsigned>{1,2}));
    });
    run("reservation before PostMessage survives UI consume before worker resumes",[](Fixture& f){
        f.prime();f.peer->push(ID_TURN,1);
        std::thread worker([&](){CHECK(f.requestPrivate());CHECK(SetEvent(f.posted));CHECK(WaitForSingleObject(f.allowReturn,5000)==WAIT_OBJECT_0);});
        f.waitPosted();f.pump();CHECK(f.wake.phase==Wake::Idle&&f.peer->count()==0);
        CHECK(f.requestPrivate());const auto next=f.wake.pending;f.release(worker);
        CHECK(f.wake.pending==next&&f.wake.phase==Wake::Queued);f.pump();
        CHECK(f.translated==2&&f.peer->deallocated==2);
    });
    run("failed Windows post releases reservation and later request makes progress",[](Fixture& f){
        f.prime();Wake::Ticket ticket{};CHECK(f.wake.reserve(&ticket));
        CHECK(!f.postTicket(ticket,reinterpret_cast<HWND>(static_cast<uintptr_t>(1))));
        CHECK(f.wake.phase==Wake::Idle);f.peer->push(ID_TURN,1);f.requestWorker();f.pump();
        CHECK(f.peer->count()==0&&f.translated==1);
    });
    run("late post failure and callback completion cannot affect reused registration",[](Fixture& f){
        f.prime();Wake::Ticket failed{};CHECK(f.wake.reserve(&failed));
        std::thread worker([&](){CHECK(!PostMessageA(reinterpret_cast<HWND>(static_cast<uintptr_t>(1)),WM_APP+2,failed.sequence,failed.generation));CHECK(SetEvent(f.posted));CHECK(WaitForSingleObject(f.allowReturn,5000)==WAIT_OBJECT_0);std::lock_guard<std::mutex> lock(f.wakeMutex);f.wake.postFailed(failed);});
        f.waitPosted();f.cancelRegistration();f.registerCurrent();CHECK(!f.wake.completed(f.object(),f.data(),failed.generation));
        f.prime();f.peer->push(ID_TURN,1);CHECK(f.requestPrivate());const auto pending=f.wake.pending;
        f.release(worker);CHECK(f.wake.pending==pending&&f.wake.phase==Wake::Queued);f.pump();CHECK(f.peer->count()==0);
    });
    run("modal delivery defers MSS and requeue changes token before outer dispatch",[](Fixture& f){
        f.prime();f.peer->push(ID_TURN,1);Wake::Ticket old{};CHECK(f.requestPrivate(&old));
        const auto wakes=f.windowWakes.load();const auto receives=f.peer->receiveCalls;f.pump(false);
        CHECK(f.wake.phase==Wake::Deferred&&f.windowWakes==wakes&&f.peer->receiveCalls==receives&&f.peer->count()==1);
        f.requestWorker(16);CHECK(f.recoveryPosts==1);f.requeueDeferred();
        CHECK(f.wake.pending!=old.sequence&&f.wake.phase==Wake::Queued);
        // A delayed duplicate of the consumed modal message must not steal the replacement.
        MSG stale{};stale.hwnd=f.window;stale.message=WM_APP+2;stale.wParam=old.sequence;stale.lParam=old.generation;
        CHECK(!f.preparePrivate(stale));CHECK(f.wake.phase==Wake::Queued);f.pump();
        CHECK(f.windowWakes==wakes+1&&f.peer->count()==0&&f.translated==1);
    });
    run("stale queued token cannot dispatch or clear a newly armed generation",[](Fixture& f){
        f.prime();Wake::Ticket old{};CHECK(f.requestPrivate(&old));f.cancelRegistration();f.registerCurrent();
        f.prime();f.peer->push(ID_TURN,1);CHECK(f.requestPrivate());CHECK(!f.wake.consume(old));
        f.pump();CHECK(f.peer->count()==0&&f.stale==1&&f.translated==1);
    });
    run("exact event removal cancels a deferred request including event ID zero",[](Fixture& f){
        f.prime();f.peer->push(ID_TURN,1);f.requestWorker();f.pump(false);CHECK(f.wake.phase==Wake::Deferred);
        CHECK(!f.wake.removed(f.object(),1));CHECK(f.wake.removed(f.object(),0));
        const auto wakes=f.windowWakes.load();f.pump();f.requestWorker();f.pump();CHECK(f.windowWakes==wakes&&f.peer->count()==1);
        f.registerCurrent();CHECK(!f.wake.armed);f.workerTicks(1);f.pump();CHECK(f.peer->count()==0);
    });
    run("duplicate registration fails closed and suppresses previously queued work",[](Fixture& f){
        f.prime();f.peer->push(ID_TURN,1);f.requestWorker();const auto wakes=f.windowWakes.load();
        CHECK(!f.wake.registered(f.object(),f.data(),1));CHECK(f.wake.unavailable);
        f.pump();f.requestWorker();CHECK(f.windowWakes==wakes&&f.peer->count()==1);
    });
    run("sequence overflow cannot reuse a delayed token",[](Fixture& f){
        f.prime();Wake::Ticket old{};CHECK(f.requestPrivate(&old));f.pump();f.wake.sequence=UINT_MAX;
        f.peer->push(ID_TURN,1);CHECK(!f.requestPrivate());CHECK(f.wake.unavailable&&!f.wake.consume(old));
        f.pump();CHECK(f.peer->count()==1);
    });
    run("generation overflow prevents pointer and event ID reuse",[](Fixture& f){
        f.prime();Wake::Ticket old{};CHECK(f.requestPrivate(&old));f.wake.generation=UINT_MAX;f.wake.cancel();
        CHECK(f.wake.unavailable&&!f.wake.registered(f.object(),f.data(),0)&&!f.wake.consume(old));f.pump();CHECK(f.translated==0);
    });
    run("recovery cannot force a blocked callback and does not recurse into it",[](Fixture& f){
        f.prime();f.service->recorder.entered=f.posted;f.service->recorder.allowReturn=f.allowReturn;
        f.peer->push(ID_TURN,1);f.workerTicks(1);
        std::thread releaser([&](){f.waitPosted();f.requestPrivate();f.requestPrivate();CHECK(f.recoveryPosts==1);CHECK(f.windowWakes==2);CHECK(SetEvent(f.allowReturn));});
        // UI stays inside the real extracted callback until the other thread releases it.
        // Private posting cannot execute it concurrently or manufacture a completion.
        f.pump();releaser.join();f.service->recorder.entered=nullptr;f.service->recorder.allowReturn=nullptr;
        CHECK(f.windowWakes==3&&f.translated==1&&f.peer->count()==0);
        CHECK((f.service->recorder.sequences==std::vector<unsigned>{0,1}));
    });
    UnregisterClassA(wc.lpszClassName,wc.hInstance);
    std::printf("RESULT=%s cases=%u failures=%u; production WakeState, extracted MSS methods, real Win32 posts and threads; native boundary classification is a fixture input\n",failures?"FAIL":"PASS",cases,failures.load());
    return failures?1:0;
}

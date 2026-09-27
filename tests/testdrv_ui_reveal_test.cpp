#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <iostream>
using DWORD = unsigned long;
namespace game { struct CDialogInterf {}; }
struct DlgEntry { game::CDialogInterf* ptr; std::uint32_t ownerInstance; void* screen; DWORD firstBindTick; bool firstBindTickSet; };
game::CDialogInterf modal, map;
game::CDialogInterf* g_curDialog = &modal;
std::uint32_t g_curOwnerInstance=9, g_dialogInstance=10;
DWORD g_curOwnerFirstBindTick=100;
bool g_curOwnerFirstBindTickSet=true;
char g_lastDialog[48]="DLG_MESSAGE_BOX";
DlgEntry entry{&map,7,reinterpret_cast<void*>(1),70,true};
bool available=true;
DlgEntry* findEntry(const char*) { return available ? &entry : nullptr; }
void advanceCounter(std::uint32_t& v,unsigned,const char*) { ++v; }
void lstrcpynA(char* dest,const char* value,int size) { std::strncpy(dest,value,size);dest[size-1]=0; }
#include "testdrv_ui_reveal_production.inc"
void check(bool value) { if(!value) throw std::runtime_error("atomic reveal invariant"); }
int main() {
    auto original=entry;
    auto rejected=[&] {
        check(!selectRevealedDialog(reinterpret_cast<void*>(1),"DLG_ISO_PAL"));
        check(std::strcmp(g_lastDialog,"DLG_MESSAGE_BOX")==0 && g_dialogInstance==10
            && g_curOwnerInstance==9 && g_curDialog==&modal && g_curOwnerFirstBindTick==100);
        entry=original;available=true;
    };
    available=false;rejected();
    entry.ptr=nullptr;rejected();
    entry.ownerInstance=0;rejected();
    entry.screen=reinterpret_cast<void*>(2);rejected();
    entry.ptr=&modal;entry.ownerInstance=9;rejected();
    check(selectRevealedDialog(reinterpret_cast<void*>(1),"DLG_ISO_PAL"));
    check(std::strcmp(g_lastDialog,"DLG_ISO_PAL")==0 && g_dialogInstance==11
        && g_curOwnerInstance==7 && g_curDialog==&map && g_curOwnerFirstBindTick==70);
    check(!selectRevealedDialog(reinterpret_cast<void*>(1),"DLG_STRATEGIC"));
    check(std::strcmp(g_lastDialog,"DLG_ISO_PAL")==0 && g_dialogInstance==11);
    std::cout<<"PASS: actual atomic revealed-dialog selection (7 cases)\n";
}

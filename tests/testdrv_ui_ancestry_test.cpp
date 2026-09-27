#include <iostream>
#include <stdexcept>
#include <vector>
#define __try try
#define __except(x) catch (...)
namespace game {
struct CInterface;
struct CInterfaceData { CInterface* parent = nullptr; };
struct Vft {
    int (*getChildsCount)(const CInterface*);
    CInterface* (*getChild)(const CInterface*, const int*);
};
struct CInterface { Vft* vftable = nullptr; CInterfaceData* interfaceData = nullptr; std::vector<CInterface*> children; };
struct CDialogInterf : CInterface {};
}
#include "testdrv_ui_ancestry_production.inc"
int main() {
    using namespace game;
    int checks=0;
    auto check=[&](bool value) {++checks;if(!value) throw std::runtime_error("ancestry invariant");};
    Vft vft{[](const CInterface* n){return static_cast<int>(n->children.size());},
            [](const CInterface* n,const int* i){return n->children.at(*i);}};
    CInterface top, other, wrapper; CDialogInterf dialog;
    CInterfaceData childData{&wrapper}, wrapperData{&top};
    top.vftable=wrapper.vftable=&vft; wrapper.interfaceData=&wrapperData;
    dialog.interfaceData=&childData; top.children={&wrapper}; wrapper.children={&dialog};
    check(observeDialogOnScreen(&dialog,&top));
    check(observeDialogOnScreen(&dialog,&wrapper));
    check(!observeDialogOnScreen(&dialog,&other));
    check(!observeDialogOnScreen(nullptr,&top));
    check(!observeDialogOnScreen(&dialog,nullptr));
    top.children.clear(); check(!observeDialogOnScreen(&dialog,&top)); top.children={&wrapper};
    wrapper.children={&dialog,&dialog}; check(!observeDialogOnScreen(&dialog,&top)); wrapper.children={&dialog};
    wrapperData.parent=&wrapper; check(!observeDialogOnScreen(&dialog,&top)); wrapperData.parent=&top;
    dialog.interfaceData=nullptr; check(!observeDialogOnScreen(&dialog,&top)); dialog.interfaceData=&childData;
    wrapper.vftable=nullptr; check(!observeDialogOnScreen(&dialog,&top)); wrapper.vftable=&vft;
    wrapper.children.assign(257,&dialog); check(!observeDialogOnScreen(&dialog,&top)); wrapper.children={&dialog};
    // Reciprocal multi-node cycle is bounded, not a successful root observation.
    CInterfaceData topData{&wrapper}; top.interfaceData=&topData; wrapper.children.push_back(&top);
    check(!observeDialogOnScreen(&dialog,&other));
    vft.getChild=[](const CInterface*,const int*)->CInterface* {throw std::runtime_error("simulated native read fault");};
    check(!observeDialogOnScreen(&dialog,&top));
    std::cout<<"PASS: actual native UI ancestry helper ("<<checks<<" cases)\n";
}

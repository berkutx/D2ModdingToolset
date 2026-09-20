#include "../slicedcapture.h"
#include <iostream>

int runSlicedStateTests()
{
    struct Example { const char* label; int kind, local, selection, animation, playback; bool accepted; };
    const Example cases[] = {
        {"observed first PvE choice: local=1 selection=1 anim=1 playback=-1", 2,1,1,1,-1,true},
        {"accepted first PvP choice", 1,1,1,1,-1,true},
        {"ordinary ready local choice", 2,1,1,0,-1,true},
        {"ready opponent turn remains observable", 2,0,0,0,-1,true},
        {"ready spectator remains observable", 1,-1,0,0,-1,true},
        {"submit closed selection before animation", 2,1,0,1,-1,false},
        {"local Result playback", 2,1,0,1,1,false},
        {"remote Result playback", 2,0,0,1,0,false},
        {"playback contradicts open selection", 2,1,1,1,1,false},
        {"remote playback contradicts open selection", 2,1,1,1,0,false},
        {"unknown animation is never overridden", 2,1,1,-1,-1,false},
        {"unknown local owner cannot override", 2,-1,1,1,-1,false},
        {"remote owner cannot override", 2,0,1,1,-1,false},
        {"unknown selection cannot override", 2,1,-1,1,-1,false},
        {"no battle", 0,1,1,1,-1,false},
        {"no battle even with ready byte", 0,1,1,0,-1,false},
        {"unknown battle", -1,1,1,0,-1,false},
        {"unexpected animation value", 2,1,1,2,-1,false},
    };
    for (const Example& example : cases) {
        SlicedBattleState value = {1,6,example.kind,example.local,example.selection,0,example.animation,example.playback};
        if (slicedStateEligible(value) != example.accepted) {
            std::cerr << "FAIL: " << example.label << '\n'; return 1;
        }
    }
    std::cout << "18 sliced-state regression cases passed\n";
    return 0;
}

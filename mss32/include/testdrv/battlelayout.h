#ifndef TESTDRV_BATTLELAYOUT_H
#define TESTDRV_BATTLELAYOUT_H
#include <cstring>
namespace hooks { namespace testdrv {
// Resource layouts of the same native battle viewer. This only classifies a
// dialog; exact owner/functor/layout admission is still required before acting.
inline bool isBattleDialog(const char* name)
{
    return name && (std::strcmp(name, "DLG_BATTLE_A") == 0
                    || std::strcmp(name, "DLG_BATTLE_B") == 0);
}
}}
#endif

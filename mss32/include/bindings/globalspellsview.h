#ifndef GLOBALSPELLSVIEW_H
#define GLOBALSPELLSVIEW_H

#include <optional>
#include <string>

namespace sol {
class state;
}

namespace bindings {

class SpellView;

class GlobalSpellsView
{
public:
    static void bind(sol::state& lua);

    std::optional<SpellView> get(const std::string& idStr) const;
};

} // namespace bindings

#endif
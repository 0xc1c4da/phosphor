#include "core/colour_system.h"

namespace phos::colour
{
ColourSystem& GetColourSystem()
{
    static ColourSystem g;
    return g;
}
} // namespace phos::colour



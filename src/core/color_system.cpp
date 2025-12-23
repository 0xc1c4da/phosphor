#include "core/color_system.h"

namespace phos::color
{
ColorSystem& GetColorSystem()
{
    static ColorSystem g;
    return g;
}
} // namespace phos::color



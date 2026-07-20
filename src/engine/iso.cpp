#include "engine/iso.h"

#include <algorithm>

namespace iso {

void DrawList::flush(DrawCtx& ctx) {
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.key < b.key; });
    for (const Item& it : items)
        draw_sprite(ctx, *it.sheet, it.frame, it.x, it.y, it.light, it.flip);
    items.clear();
}

}  // namespace iso

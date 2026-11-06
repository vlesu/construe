// "Through all" depth (spec/kernel-interface.md §4.5): a common algorithm on top of the bbox query.
#include "kernel/kernel.h"

#include <algorithm>
#include <array>

namespace construe::kernel {

double throughAllDepth(const Kernel& k, const Shape& bounds, const Point3& origin, const Vec3& direction) {
    const BBox3 b = k.bbox(bounds);
    const Vec3 d = direction.normalized();
    double maxS = 0;
    for (int i = 0; i < 8; ++i) {
        const Point3 c((i & 1) ? b.max().x() : b.min().x(), (i & 2) ? b.max().y() : b.min().y(),
                       (i & 4) ? b.max().z() : b.min().z());
        maxS = std::max(maxS, (c - origin).dot(d));
    }
    const double margin = std::max(KERNEL_THROUGH_ALL_MARGIN_ABS, KERNEL_THROUGH_ALL_MARGIN_REL * (b.max() - b.min()).norm());
    return maxS + margin;
}

}  // namespace construe::kernel

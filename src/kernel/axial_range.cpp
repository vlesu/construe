// Axial extent of a face (spec/kernel-interface.md §4.6): a common algorithm on top of canonicalTopology and edgeCurve.
#include "kernel/kernel.h"

#include <algorithm>

namespace construe::kernel {

std::optional<std::pair<double, double>> axialRange(const Kernel& k, const Shape& sh, int face, const Point3& p, const Vec3& a) {
    double t0 = 1e300, t1 = -1e300;
    int n = 0;
    const auto ct = canonicalTopology(k, sh);
    for (size_t le = 0; le < ct.logicalEdges.size(); ++le) {
        const auto& L = ct.logicalEdges[le];
        if (L.faces.first != face && L.faces.second != face) continue;
        const auto cv = k.edgeCurve(sh, static_cast<int>(le));
        if (cv.kind != CurveInfo::Kind::Circle) continue;
        const double t = (cv.center - p).dot(a);
        t0 = std::min(t0, t), t1 = std::max(t1, t), ++n;
    }
    if (n < 2 || !(t1 > t0)) return std::nullopt;
    return std::pair<double, double>{t0, t1};
}

}  // namespace construe::kernel

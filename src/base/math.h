// Core math types (spec/base.md §2). Units: mm, radians.
#pragma once

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace construe {

using Vec2 = Eigen::Vector2d;
using Vec3 = Eigen::Vector3d;
using Point2 = Eigen::Vector2d;
using Point3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;
using Transform3 = Eigen::Isometry3d;
using BBox3 = Eigen::AlignedBox3d;

// Plane with an orthonormal basis: normal = x × y.
struct Plane {
    Point3 origin = Point3::Zero();
    Vec3 x = Vec3::UnitX();
    Vec3 y = Vec3::UnitY();
    Vec3 normal = Vec3::UnitZ();

    // Local coordinates (u, v) → world: origin + u·x + v·y.
    Point3 toWorld(const Point2& uv) const { return origin + uv.x() * x + uv.y() * y; }

    // World point → local coordinates (projection onto the plane).
    Point2 toLocal(const Point3& p) const {
        const Vec3 d = p - origin;
        return {d.dot(x), d.dot(y)};
    }
};

}  // namespace construe

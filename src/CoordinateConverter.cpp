#include "CoordinateConverter.h"

#include <cmath>

namespace GoldSrcAssetConverter
{
constexpr double s_Pi            = 3.14159265358979323846;
constexpr double s_HalfTurn      = 180.0;
constexpr double s_Half          = 0.5;
constexpr double s_GLTFRotationX = -90.0;

//Writes zero components as +0: a zero axis component times a negative sine is -0, which documents would keep.
static double PositiveZero(double value)
{
    return value == 0 ? 0.0 : value;
}

Quaternion MultiplyQuaternions(const Quaternion& a, const Quaternion& b)
{
    return {
        PositiveZero(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y),
        PositiveZero(a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x),
        PositiveZero(a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w),
        PositiveZero(a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z),
    };
}

Quaternion MakeAxisRotation(double angle, double axisX, double axisY, double axisZ)
{
    const auto halfAngle = angle * s_Pi / s_HalfTurn * s_Half;
    const auto sine      = std::sin(halfAngle);

    return {PositiveZero(axisX * sine), PositiveZero(axisY * sine), PositiveZero(axisZ * sine), PositiveZero(std::cos(halfAngle))};
}

Quaternion MakeGoldSrcAnglesRotation(const GoldSrcRender::Vector3& angles)
{
    const auto yaw   = MakeAxisRotation(angles.y, 0, 0, 1);
    const auto pitch = MakeAxisRotation(angles.x, 0, 1, 0);
    const auto roll  = MakeAxisRotation(angles.z, 1, 0, 0);

    return MultiplyQuaternions(MultiplyQuaternions(yaw, pitch), roll);
}

Quaternion MakeGoldSrcToGLTFRotation()
{
    return MakeAxisRotation(s_GLTFRotationX, 1, 0, 0);
}

bool IsIdentityRotation(const Quaternion& rotation)
{
    return rotation.x == 0 && rotation.y == 0 && rotation.z == 0 && rotation.w == 1;
}
} // namespace GoldSrcAssetConverter

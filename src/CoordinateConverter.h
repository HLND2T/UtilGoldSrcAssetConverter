#if !defined(COORDINATE_CONVERTER_H)
#    define COORDINATE_CONVERTER_H

#    ifdef _WIN32
#        pragma once
#    endif

#    include "GoldSrcRenderBaseTypes.h"

//GoldSrc coordinates are right-handed with z up and x forward; glTF's are right-handed with y up. Vertices and entity
//transforms keep GoldSrc coordinates and units; the root node turns them into glTF's with a rotation of -90 degrees
//about x, (x, y, z) -> (x, z, -y), which keeps handedness and face winding, and the conversion's scale factor.
namespace GoldSrcAssetConverter
{
//Unit quaternion in glTF component order x, y, z, w.
struct Quaternion
{
    double x{};
    double y{};
    double z{};
    double w{1};
};

Quaternion MultiplyQuaternions(const Quaternion& a, const Quaternion& b);

//Rotation by angle degrees counterclockwise about a unit axis, as glRotatef.
Quaternion MakeAxisRotation(double angle, double axisX, double axisY, double axisZ);

//The rotation the engine's R_GetEntityTransform applies for angles (pitch, yaw, roll): yaw about z, then pitch about
//y, then roll about x, each as glRotatef, in that order of matrix products.
Quaternion MakeGoldSrcAnglesRotation(const GoldSrcRender::Vector3& angles);

//The root node's rotation from GoldSrc to glTF coordinates.
Quaternion MakeGoldSrcToGLTFRotation();

bool IsIdentityRotation(const Quaternion& rotation);
} // namespace GoldSrcAssetConverter

#endif

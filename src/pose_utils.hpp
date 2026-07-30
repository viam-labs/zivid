#pragma once

// Shared pose conversion utilities between zivid_handeye.cpp and zivid_stitcher.cpp.
// All functions are inline — no separate translation unit needed.

#include <array>
#include <cmath>

#include <Zivid/Matrix.h>
#include <viam/sdk/components/arm.hpp>

namespace viam_zivid {

struct ViamOV {
    double ox, oy, oz, theta_deg;
    double tx, ty, tz;
};

// Convert Viam OrientationVector (degrees) + position (mm) to a Zivid::Matrix4x4.
//
// Viam's OV representation: (o_x, o_y, o_z) is a unit vector defining the end-effector's
// pointing direction (Z-axis); theta (degrees) is the roll around that direction.
// Conversion mirrors go.viam.com/rdk/spatialmath: OV → ZYZ Euler → quaternion → R matrix.
inline Zivid::Matrix4x4 viam_pose_to_zivid(const viam::sdk::pose& p) {
    // Translation (mm — Viam arm poses are in mm, matching Zivid's convention).
    const double tx = p.coordinates.x;
    const double ty = p.coordinates.y;
    const double tz = p.coordinates.z;

    // Normalize orientation vector.
    double ox = p.orientation.o_x;
    double oy = p.orientation.o_y;
    double oz = p.orientation.o_z;
    const double norm = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (norm > 1e-10) {
        ox /= norm;
        oy /= norm;
        oz /= norm;
    } else {
        ox = 0.0;
        oy = 0.0;
        oz = 1.0;
    }

    // ZYZ Euler angles.
    const double lat = std::acos(std::max(-1.0, std::min(1.0, oz)));
    const double lon = (1.0 - std::abs(oz) > 1e-8) ? std::atan2(oy, ox) : 0.0;
    const double th = p.theta * M_PI / 180.0;

    // Build quaternion q = Rz(lon) * Ry(lat) * Rz(th).
    //   Rz(a) = (cos(a/2),  0,         0,        sin(a/2))  as (w,x,y,z)
    //   Ry(a) = (cos(a/2),  0,         sin(a/2), 0        )
    const double clon2 = std::cos(lon / 2), slon2 = std::sin(lon / 2);
    const double clat2 = std::cos(lat / 2), slat2 = std::sin(lat / 2);
    const double cth2 = std::cos(th / 2), sth2 = std::sin(th / 2);

    // q1 = Rz(lon) * Ry(lat)
    const double q1w = clon2 * clat2;
    const double q1x = -slon2 * slat2;
    const double q1y = clon2 * slat2;
    const double q1z = slon2 * clat2;

    // q = q1 * Rz(th)  where Rz(th) = (cth2, 0, 0, sth2)
    const double qw = q1w * cth2 - q1z * sth2;
    const double qx = q1x * cth2 + q1y * sth2;
    const double qy = q1y * cth2 - q1x * sth2;
    const double qz = q1w * sth2 + q1z * cth2;

    // Quaternion → 3×3 rotation matrix (standard unit-quat formula).
    const double r00 = 1 - 2 * qy * qy - 2 * qz * qz;
    const double r01 = 2 * qx * qy - 2 * qw * qz;
    const double r02 = 2 * qx * qz + 2 * qw * qy;
    const double r10 = 2 * qx * qy + 2 * qw * qz;
    const double r11 = 1 - 2 * qx * qx - 2 * qz * qz;
    const double r12 = 2 * qy * qz - 2 * qw * qx;
    const double r20 = 2 * qx * qz - 2 * qw * qy;
    const double r21 = 2 * qy * qz + 2 * qw * qx;
    const double r22 = 1 - 2 * qx * qx - 2 * qy * qy;

    // clang-format off
    const std::array<float, 16> elements = {
        static_cast<float>(r00), static_cast<float>(r01), static_cast<float>(r02), static_cast<float>(tx),
        static_cast<float>(r10), static_cast<float>(r11), static_cast<float>(r12), static_cast<float>(ty),
        static_cast<float>(r20), static_cast<float>(r21), static_cast<float>(r22), static_cast<float>(tz),
        0.f, 0.f, 0.f, 1.f
    };
    // clang-format on
    return Zivid::Matrix4x4{elements.begin(), elements.end()};
}

// Convert a Zivid::Matrix4x4 (4×4 transform, translation in mm) back to Viam's OV format.
// Inverse of viam_pose_to_zivid — used to produce a frame-system compatible result.
inline ViamOV zivid_to_viam_ov(const Zivid::Matrix4x4& mat) {
    // Pointing direction = third column of R (where the Z-axis maps to).
    const double ox = static_cast<double>(mat(0, 2));
    const double oy = static_cast<double>(mat(1, 2));
    const double oz_val = static_cast<double>(mat(2, 2));

    const double lat = std::acos(std::max(-1.0, std::min(1.0, oz_val)));
    const double lon = (1.0 - std::abs(oz_val) > 1e-8) ? std::atan2(oy, ox) : 0.0;

    // Build M = Ry(-lat) * Rz(-lon) and compute theta from [M*R][1][0] / [M*R][0][0].
    const double clat = std::cos(lat), slat = std::sin(lat);
    const double clon = std::cos(lon), slon = std::sin(lon);

    // M rows: row0 = [clat*clon, clat*slon, -slat]
    //         row1 = [-slon,     clon,       0   ]
    // Only the first column of R is needed.
    const double r00 = static_cast<double>(mat(0, 0));
    const double r10 = static_cast<double>(mat(1, 0));
    const double r20 = static_cast<double>(mat(2, 0));

    const double mr00 = clat * clon * r00 + clat * slon * r10 + (-slat) * r20;
    const double mr10 = (-slon) * r00 + clon * r10;

    const double theta_deg = std::atan2(mr10, mr00) * 180.0 / M_PI;

    return {ox, oy, oz_val, theta_deg, static_cast<double>(mat(0, 3)), static_cast<double>(mat(1, 3)), static_cast<double>(mat(2, 3))};
}

}  // namespace viam_zivid

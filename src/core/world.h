// DupliX core: the effect's World Transform (both modes). No After Effects dependency.
// Matrices are row vectors (p' = p * M), row 3 = translation, like AEGP and detail::MakeM4.
#pragma once

namespace rnd {

struct WorldXform {
    double pos[3] = {0, 0, 0};          // px, an offset in layer space
    double rotDeg[3] = {0, 0, 0};       // X, Y, Z
    double scale[3] = {100, 100, 100};  // percent
};

// G = T(-A) * S * Rz * Ry * Rx * T(A + P), A = (ax, ay, 0): the same order and signs as a copy's
// detail::MakeM4 (AE's Rotate X/Y/Z). The default WorldXform gives the identity.
// The copies are placed by copyM4 * G * layerToWorld.
void WorldM4(const WorldXform& w, double ax, double ay, double out[16]);

// The inverse of WorldM4: scale = row lengths of the 3x3 part (Z negative for a mirror), rotation from the
// normalised rows (Z = 0 at gimbal lock). A shear has no exact WorldXform: the nearest is returned.
// False if the last column is not (0, 0, 0, 1) or the 3x3 part is singular.
bool DecomposeWorld(const double G[16], double ax, double ay, WorldXform* out);

// Layer -> world of a 3D layer in its initial state (anchor A at the comp centre, z 0, no rotation, 100%):
// T(-A) * T(compW / 2, compH / 2, 0). World coordinates are comp px (AEGP_GetLayerToWorldXform).
void InitialLayerToWorld(double ax, double ay, double compW, double compH, double out[16]);

// Fast mode switched on: G_new = G_cur * layerToWorld * V^-1 (V = InitialLayerToWorld), so that
// copies * G_new * V == copies * G_cur * layerToWorld. False if the result cannot be decomposed.
bool TransferToWorld(const WorldXform& cur, const double layerToWorld[16], double ax, double ay, double compW,
                     double compH, WorldXform* out);

}  // namespace rnd

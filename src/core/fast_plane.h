// DupliX core: geometry of the fast mode (all copies drawn into one image on a screen-aligned plane).
// No After Effects dependency. Matrices are row vectors (p' = p * M), row 3 = translation, like AEGP.
#pragma once

namespace rnd {

// The comp's camera as AEGP_GetEffectCameraMatrix reports it.
// Projection: comp px = camera-space xy * dist / z + (compW, compH) / 2.
struct CameraView {
    double camToWorld[16];
    double dist;
    double compW, compH;
};

bool Invert4(const double a[16], double out[16]);
void Mul4(const double a[16], const double b[16], double out[16]);
bool Invert3(const double a[9], double out[9]);

// The sub-layer matrix (layer px -> layer px) of a plane facing the camera at camera-space depth `depth`
// that shows layer px (x, y) at comp px (x, y): M4 = S * C * W^-1 (spec 12.10).
bool PlaneM4(const CameraView& cam, const double layerToWorld[16], double depth, double out[16]);

// Camera-space z of the layer point (ax, ay, 0).
double AnchorDepth(const CameraView& cam, const double layerToWorld[16], double ax, double ay);

// Screen px -> source layer px (3x3, row vectors, homogeneous: [sx sy 1] * H = w * [u v 1]) of a copy
// whose sub-layer matrix is copyM4. srcRect = {left, top, right, bottom} in layer px.
// False when a corner of srcRect is at camera-space z <= 1 (at or behind the camera): the copy is not drawn.
bool CopyHomography(const CameraView& cam, const double layerToWorld[16], const double copyM4[16],
                    const double srcRect[4], double screenToSrc[9]);

// The screen bounds {left, top, right, bottom} (full-resolution comp px) of the copy's srcRect. False when a corner
// is at camera-space z <= 1 (the copy is not drawn, as for CopyHomography).
bool CopyScreenBounds(const CameraView& cam, const double layerToWorld[16], const double copyM4[16],
                      const double srcRect[4], double out[4]);

// Camera-space z of the copy's point (cx, cy) (layer px), e.g. its centre: the drawing order.
double CopyDepth(const CameraView& cam, const double layerToWorld[16], const double copyM4[16], double cx, double cy);

}  // namespace rnd

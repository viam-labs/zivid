#pragma once

#include <mutex>

namespace viam_zivid {

// Serializes the projector-on window of every Zivid camera in this process.
//
// Zivid cameras are structured-light: a 3D capture floods the scene with a projected
// pattern and decodes the reflection. Two cameras whose fields of view overlap corrupt
// each other's pattern if they acquire at the same time, which shows up as noise and
// dropouts in the overlap region, so Zivid requires capturing with one camera at a
// time. Hold this around Zivid capture calls only — not around point-cloud retrieval
// or copying, which happen after the projector is off and can safely overlap.
//
// Scope is this module process, which is where every viam:zivid:camera component of a
// single module instance lives. Cameras driven from another process are not covered.
//
// https://support.zivid.com/en/latest/camera/academy/camera/multi-zivid/multiple-cameras-performance-considerations.html
inline std::mutex& capture_lock() {
    static std::mutex lock;
    return lock;
}

// Serializes camera enumeration, connection and disconnection.
//
// Zivid requires that cameras be listed and connected sequentially, and that no camera
// be operated while another thread is inside Application::cameras() or
// Camera::connect(). Without this, reconfiguring two camera components at once races.
//
// https://support.zivid.com/en/latest/camera/academy/camera/capture-tutorial/multithreading.html
inline std::mutex& device_lock() {
    static std::mutex lock;
    return lock;
}

}  // namespace viam_zivid

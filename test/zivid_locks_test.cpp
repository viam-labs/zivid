#include <mutex>

#include <gtest/gtest.h>

#include "zivid_locks.hpp"

// Defined in zivid_locks_other_tu.cpp.
namespace test_other_tu {
std::mutex* capture_lock_address();
std::mutex* device_lock_address();
}  // namespace test_other_tu

namespace {

// The locks only serialize anything if every translation unit resolves to the same
// mutex object. zivid_camera.cpp and zivid_discovery.cpp both include the header, so a
// per-TU copy would silently let two cameras capture at once.
TEST(ZividLocks, SameMutexAcrossTranslationUnits) {
    EXPECT_EQ(&viam_zivid::capture_lock(), test_other_tu::capture_lock_address());
    EXPECT_EQ(&viam_zivid::device_lock(), test_other_tu::device_lock_address());
}

TEST(ZividLocks, CaptureAndDeviceLocksAreDistinct) {
    EXPECT_NE(&viam_zivid::capture_lock(), &viam_zivid::device_lock());
}

}  // namespace

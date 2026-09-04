// Second translation unit for the cross-TU checks in zivid_locks_test.cpp.

#include "zivid_locks.hpp"

namespace test_other_tu {

std::mutex* capture_lock_address() {
    return &viam_zivid::capture_lock();
}

std::mutex* device_lock_address() {
    return &viam_zivid::device_lock();
}

}  // namespace test_other_tu

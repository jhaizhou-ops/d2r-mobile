#include <cassert>

__attribute__((noinline)) static void throw_on_guest_stack() {
    throw 123;
}

extern "C" void dm_windows_test_stack_exception() {
    try {
        throw_on_guest_stack();
        assert(false);
    } catch (int value) {
        assert(value == 123);
    }
}

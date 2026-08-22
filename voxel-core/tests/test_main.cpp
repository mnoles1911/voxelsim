#include "vxctest.h"

// An optional substring filter. The whole suite is a five-minute run, which is
// the right cost for CI and the wrong one for the edit-compile-check loop on a
// single file -- and a loop that costs five minutes is a loop that gets skipped.
//
//     ./vxc_tests               every test, exactly as CI runs it
//     ./vxc_tests colour patch  every test whose name contains either
//
// ctest still invokes it with no arguments, so the gate is unchanged.
int main(int argc, char** argv) {
    return vxctest::runAll(argc - 1, argv + 1);
}

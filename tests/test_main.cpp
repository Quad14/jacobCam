// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_harness.h"

#include "qcam/log.h"

int main(int argc, char** argv) {
    // Keep the test output readable; pass -v to see driver logging.
    bool verbose = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "-v") verbose = true;
    qcam::SetLogLevel(verbose ? qcam::LogLevel::Trace : qcam::LogLevel::Error);

    std::printf("running qcam core tests\n");
    return qcam_test::RunAll(argc, argv);
}

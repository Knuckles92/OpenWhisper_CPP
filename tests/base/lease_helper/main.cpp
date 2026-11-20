// Holds a shared data lease on argv[1] until standard input closes, as the reader process of
// OpenWhisper's tests/test_backup_startup.py does. Prints READY once the lease is held.

#include <exception>
#include <iostream>
#include <string>

#include "base/data_lease.h"
#include "base/identity.h"
#include "base/utf8.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fv_base_lease_helper <data root>\n";
        return 2;
    }
    try {
        auto lease = fv::DataLease::acquire(fv::path_from_utf8(argv[1]), fv::identity(),
                                            fv::LeaseMode::Shared);
        std::cout << "READY" << std::endl;
        std::string line;
        while (std::getline(std::cin, line)) {
        }
        lease.release();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

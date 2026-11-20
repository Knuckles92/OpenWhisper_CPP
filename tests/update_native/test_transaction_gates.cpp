#include "update/release_identity.h"
#include "update/testing/fake_mutex_api.h"
#include "update_native/transaction_gates.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("native transaction gates retain control while health releases application",
          "[update_native]") {
    fv::update::testing::FakeMutexApi mutex;
    const auto& identity = fv::update::release_identity();
    {
        auto gates = fv::update_native::NativeTransactionGates::acquire(mutex, identity);
        REQUIRE(gates);
        CHECK(mutex.owned().size() == identity.update_mutexes().size() + 1);
        REQUIRE(gates->acquire_application());
        CHECK(mutex.owned().size() ==
              identity.update_mutexes().size() + identity.app_mutexes().size() + 1);
        gates->release_application();
        CHECK(mutex.owned().size() == identity.update_mutexes().size() + 1);
        mutex.held_elsewhere.insert(identity.app_mutexes().front());
        CHECK_FALSE(gates->acquire_application());
        CHECK(mutex.owned().size() == identity.update_mutexes().size() + 1);
    }
    CHECK(mutex.owned().empty());
}

TEST_CASE("native transaction gates release partially acquired control mutexes",
          "[update_native]") {
    fv::update::testing::FakeMutexApi mutex;
    const auto& identity = fv::update::release_identity();
    mutex.held_elsewhere.insert(identity.setup_mutex());
    CHECK_FALSE(fv::update_native::NativeTransactionGates::acquire(mutex, identity));
    CHECK(mutex.owned().empty());
    auto setup = fv::update_native::NativeTransactionGates::acquire(
        mutex, identity, fv::update_native::SetupGate::HeldByInstaller);
    REQUIRE(setup);
    REQUIRE(setup->acquire_application());
    CHECK(mutex.owned().size() == identity.update_mutexes().size() + identity.app_mutexes().size());
}

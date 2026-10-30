#include "store_contract.hpp"
#include <iostream>

int main() {
    try {
        std::int64_t now = 1000;
        tgpp::ext::MemoryStores stores([&] { return now; });
        store_contract::verify(stores, stores, now);
        store_contract::verify_inbox(stores, now);
        store_contract::verify_competing(stores, stores);
        store_contract::verify_session_fence(stores, stores, now);
        std::cout << "Memory store contracts passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

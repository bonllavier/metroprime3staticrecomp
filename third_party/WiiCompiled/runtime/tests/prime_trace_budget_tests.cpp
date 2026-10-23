#include "hle/prime_trace_budget.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

static void Check(bool ok) {
    if (!ok) throw std::runtime_error("Prime informational trace budget contract failed");
}
int main() {
    PrimeTraceBudget budget(128);
    std::atomic<unsigned> accepted{0};
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 10000; ++i)
            if (budget.Take(false)) accepted.fetch_add(1, std::memory_order_relaxed);
    });
    for (auto& worker : workers) worker.join();
    Check(accepted == 128);
    Check(!budget.Take(false));
    Check(budget.Take(true)); // Explicit verbose mode survives exhaustion.
    Check(!budget.Take(false)); // Verbose mode does not replenish the budget.
    PrimeTraceBudget empty(0);
    Check(!empty.Take(false) && empty.Take(true));
    PrimeTraceBudget one(1);
    Check(one.Take(true) && one.Take(false) && !one.Take(false));
    std::cout << "PASS concurrent limit, saturation, zero budget and verbose override\n";
}

// Directly reproduces a real reported usage pattern: N OS threads, each
// modeling one DMA core, calling straight into the same ddrt_engine_t*
// concurrently -- some threads even call ddrt_run() themselves mid-stream.
// Before per-handle locking was added, this raced on results()/summary()'s
// internal std::vector/std::map/std::priority_queue and could silently lose
// transactions from the final counts. Goes through the public C API
// (ddrtiming.h), not the C++ Engine class, since that's what a real
// multi-threaded caller actually links against.
#include "testing.hpp"
#include "ddrtiming/ddrtiming.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

DDRTEST(concurrent_pushes_from_multiple_threads_lose_nothing) {
    // Minimal valid config file, written fresh so this test has no external
    // file dependency.
    const char* config_path = "concurrency_test_config.json";
    {
        FILE* f = fopen(config_path, "w");
        DDR_CHECK(f != nullptr);
        fputs(R"({
          "topology": {"channels": 2, "bankgroups": 2, "banks_per_group": 2,
                        "rows": 65536, "data_bus_bytes": 8, "clock_mhz": 1600},
          "address_mapping": {
            "bank": {"bit_start": 10, "bit_width": 1},
            "bankgroup": {"bit_start": 11, "bit_width": 1},
            "row": {"bit_start": 12, "bit_width": 10},
            "channel": {"bit_start": 22, "bit_width": 1}
          },
          "ddrc_resources": {"command_queue_depth": 16, "max_outstanding_per_id": 8}
        })", f);
        fclose(f);
    }

    ddrt_engine_t* engine = ddrt_create(config_path);
    DDR_CHECK(engine != nullptr);

    constexpr int kThreads = 8;
    constexpr int kTxnsPerThread = 3000;
    std::atomic<int> push_failures{0};

    auto worker = [&](int core_id) {
        for (int i = 0; i < kTxnsPerThread; ++i) {
            ddrt_axi_txn_t t;
            t.core_id = core_id;
            t.type = (i % 2 == 0) ? DDRT_READ : DDRT_WRITE;
            t.axi_id = static_cast<uint32_t>(i % 3);
            t.addr = (static_cast<uint64_t>(core_id) << 24) + (static_cast<uint64_t>(i) * 64);
            t.size_bytes = 64;
            t.len_beats = 1;
            t.wstrb = nullptr;
            t.wstrb_len = 0;
            uint64_t txn_id = 0;
            if (ddrt_push_txn(engine, &t, &txn_id) != 0) push_failures++;

            // Some threads also drive run() themselves mid-stream, matching
            // a caller that ticks per-core rather than from a single owner.
            if (core_id % 2 == 0 && i % 500 == 499) {
                ddrt_run(engine);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int c = 0; c < kThreads; ++c) threads.emplace_back(worker, c);
    for (auto& th : threads) th.join();

    DDR_CHECK_EQ(push_failures.load(), 0);

    DDR_CHECK_EQ(ddrt_run(engine), 0);

    ddrt_summary_t s;
    DDR_CHECK_EQ(ddrt_get_summary(engine, &s), 0);

    uint64_t expected_txns = static_cast<uint64_t>(kThreads) * kTxnsPerThread;
    uint64_t expected_bytes = expected_txns * 64;

    DDR_CHECK_EQ(s.total_txns, expected_txns);
    DDR_CHECK_EQ(s.total_bytes, expected_bytes);
    DDR_CHECK_EQ(ddrt_get_num_results(engine), expected_txns);

    ddrt_destroy(engine);
    remove(config_path);
}

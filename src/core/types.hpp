#pragma once
#include <cstdint>
#include <vector>

namespace ddrtiming {

enum class TxnType { Read, Write };
enum class RowStatus { Hit, Conflict, Empty };

struct AxiTxn {
    uint64_t txn_id = 0;
    int core_id = 0;
    TxnType type = TxnType::Read;
    uint32_t axi_id = 0;
    uint64_t addr = 0;
    uint32_t size_bytes = 0;
    uint32_t len_beats = 0;
    std::vector<uint8_t> wstrb; // empty = full strobe
};

struct DecodedAddr {
    uint32_t channel = 0;
    uint32_t rank = 0;
    uint32_t bankgroup = 0;
    uint32_t bank = 0;
    uint32_t row = 0;
    uint64_t col = 0;
};

struct DramCommand {
    uint64_t txn_id = 0;
    int core_id = 0;
    TxnType type = TxnType::Read;
    DecodedAddr addr;
    uint32_t bytes = 0;
    uint32_t seq_in_txn = 0;
    uint32_t total_in_txn = 0;

    uint64_t arrival_cycle = 0;
    uint64_t start_cycle = 0;
    uint64_t complete_cycle = 0;
    RowStatus row_status = RowStatus::Empty;
};

struct TxnResult {
    uint64_t txn_id = 0;
    int core_id = 0;
    TxnType type = TxnType::Read;
    uint64_t addr = 0;
    uint64_t issue_cycle = 0;
    uint64_t complete_cycle = 0;
    double latency_ns = 0.0;
    RowStatus dominant_row_status = RowStatus::Empty;
    uint32_t hits = 0, conflicts = 0, empties = 0;
    uint32_t bytes = 0;      // logical: what the AXI burst actually requested
    uint32_t dram_bytes = 0; // physical: full burst-aligned bytes DRAM actually moved (>= bytes)
};

} // namespace ddrtiming

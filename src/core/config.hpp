#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ddrtiming {

// A field's bits need not be a contiguous span of the physical address -- real
// DDRC mapping tables commonly scatter a field across multiple ranges (e.g.
// bank[1:0] <- addr[15:14], bank[3:2] <- addr[11:10]). `bits[i]` names the
// physical address bit that supplies field bit `i` (LSB-first); field width is
// bits.size(). Still gather-only (no XOR-hash interleaving) -- see README.
struct AddressField {
    std::vector<int> bits;
    int width() const { return static_cast<int>(bits.size()); }
    static AddressField contiguous(int start, int width) {
        AddressField f;
        for (int i = 0; i < width; ++i) f.bits.push_back(start + i);
        return f;
    }
};

struct DdrcConfig {
    // topology
    int channels = 1;
    int ranks_per_channel = 1;
    int bankgroups = 1;
    int banks_per_group = 4;
    int rows = 1 << 16;
    int columns = 1 << 10;
    int data_bus_bytes = 8;     // per-channel data bus width, bytes transferred per beat/cycle
    // DRAM only ever transfers whole bursts of this many beats -- never
    // fewer -- so it directly sets the over-fetch/burst-efficiency
    // calculation. Default 8 matches DDR4 BL8; set to 4 for burst-chop (BC4),
    // or whatever matches the DRAM generation you're modeling (this does not
    // attempt to model DDR5's different prefetch architecture specifically --
    // pick the beat count that reproduces its actual minimum access
    // granularity for your part).
    int burst_beats = 8;
    double clock_mhz = 1600.0;

    // address mapping (gather-list bit fields, contiguous or scattered; see AddressField)
    AddressField map_channel;
    AddressField map_rank;
    AddressField map_bankgroup;
    AddressField map_bank;
    AddressField map_row;

    // timing, all in nanoseconds
    double tRCD = 13.75;
    double tRP = 13.75;
    double tRAS = 32.0;
    double tRC = 45.75;
    double tCCD_S = 2.5;   // different bank group
    double tCCD_L = 3.75;  // same bank group
    double tRRD_S = 2.5;
    double tRRD_L = 4.9;
    double tFAW = 21.0;
    double tWTR_S = 2.5;
    double tWTR_L = 7.5;
    double tRTP = 7.5;
    double tWR = 15.0;
    double tREFI = 7800.0;
    double tRFC = 350.0;
    double rd_wr_turnaround = 2.5; // extra bus turnaround ns switching read->write
    double wr_rd_turnaround = 2.5; // extra bus turnaround ns switching write->read

    // ddrc resources
    int command_queue_depth = 32;   // per channel, max in-flight commands
    // Outstanding cap applied per (core_id, axi_id) stream, not per core: AXI
    // guarantees same-ID transactions complete in order, but different IDs from
    // the same core may be independently in flight and complete out of order.
    // A log that never varies axi_id (or a caller that always passes 0) has
    // exactly one stream per core, which degenerates to a flat per-core cap.
    int max_outstanding_per_id = 16;
    std::string scheduling_policy = "fr_fcfs"; // "fr_fcfs" | "in_order"

    // reporting: bucket dispatched transactions into fixed-size windows of
    // simulated time (by issue_cycle) for a bandwidth/byte-access history,
    // independent of when/how often the caller happens to call run() and
    // unaffected by prune_results_before() -- see README "Windowed history".
    // 0 (default) disables windowed accounting entirely.
    double history_window_ns = 0.0;

    double clock_period_ns() const { return 1000.0 / clock_mhz; }
    uint64_t ns_to_cycles(double ns) const {
        double c = ns / clock_period_ns();
        uint64_t whole = static_cast<uint64_t>(c);
        return (c - static_cast<double>(whole) > 1e-9) ? whole + 1 : (whole == 0 ? 0 : whole);
    }
    double peak_bandwidth_gbps() const {
        // bytes/cycle * cycles/ns = bytes/ns == GB/s, times number of channels
        return static_cast<double>(data_bus_bytes) / clock_period_ns() * channels;
    }
    int total_banks() const {
        return std::max(1, channels) * std::max(1, ranks_per_channel) * std::max(1, bankgroups) * std::max(1, banks_per_group);
    }

    static DdrcConfig load_from_file(const std::string& path);
};

} // namespace ddrtiming

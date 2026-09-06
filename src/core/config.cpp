#include "config.hpp"
#include "json.hpp"

#include <stdexcept>
#include <utility>

namespace ddrtiming {

namespace {
// Accepts either the ergonomic contiguous form {"bit_start":8,"bit_width":2}
// or an explicit scattered form {"bits":[14,15,10,11]} (field bit0 <- addr
// bit14, bit1 <- addr bit15, bit2 <- addr bit10, bit3 <- addr bit11). Either
// form may add an optional "hash_start" (int) to XOR-hash the field against
// physical bits [hash_start, hash_start+width) -- see AddressField::xor_hashed.
AddressField parse_field(const json::Value& obj, const std::string& key) {
    AddressField f;
    if (!obj.contains(key)) return f;
    const json::Value& v = obj[key];
    if (v.contains("bits")) {
        for (const auto& item : v["bits"].array_items()) {
            f.bits.push_back(static_cast<int>(item.as_int(0)));
        }
    } else {
        int bit_start = static_cast<int>(v.get_int("bit_start", 0));
        int bit_width = static_cast<int>(v.get_int("bit_width", 0));
        f = AddressField::contiguous(bit_start, bit_width);
    }
    if (v.contains("hash_start")) {
        f = AddressField::xor_hashed(f, static_cast<int>(v.get_int("hash_start", 0)));
    }
    return f;
}
} // namespace

DdrcConfig DdrcConfig::load_from_file(const std::string& path) {
    json::Value root = json::parse_file(path);
    DdrcConfig cfg;

    if (root.contains("topology")) {
        const json::Value& t = root["topology"];
        cfg.channels = static_cast<int>(t.get_int("channels", cfg.channels));
        cfg.ranks_per_channel = static_cast<int>(t.get_int("ranks_per_channel", cfg.ranks_per_channel));
        cfg.bankgroups = static_cast<int>(t.get_int("bankgroups", cfg.bankgroups));
        cfg.banks_per_group = static_cast<int>(t.get_int("banks_per_group", cfg.banks_per_group));
        cfg.rows = static_cast<int>(t.get_int("rows", cfg.rows));
        cfg.columns = static_cast<int>(t.get_int("columns", cfg.columns));
        cfg.data_bus_bytes = static_cast<int>(t.get_int("data_bus_bytes", cfg.data_bus_bytes));
        cfg.burst_beats = static_cast<int>(t.get_int("burst_beats", cfg.burst_beats));
        cfg.clock_mhz = t.get_num("clock_mhz", cfg.clock_mhz);
    }

    if (root.contains("address_mapping")) {
        const json::Value& m = root["address_mapping"];
        cfg.map_channel = parse_field(m, "channel");
        cfg.map_rank = parse_field(m, "rank");
        cfg.map_bankgroup = parse_field(m, "bankgroup");
        cfg.map_bank = parse_field(m, "bank");
        cfg.map_row = parse_field(m, "row");
    }

    if (root.contains("timing_ns")) {
        const json::Value& tm = root["timing_ns"];
        cfg.tRCD = tm.get_num("tRCD", cfg.tRCD);
        cfg.tRP = tm.get_num("tRP", cfg.tRP);
        cfg.tRAS = tm.get_num("tRAS", cfg.tRAS);
        cfg.tRC = tm.get_num("tRC", cfg.tRC);
        cfg.tCCD_S = tm.get_num("tCCD_S", cfg.tCCD_S);
        cfg.tCCD_L = tm.get_num("tCCD_L", cfg.tCCD_L);
        cfg.tRRD_S = tm.get_num("tRRD_S", cfg.tRRD_S);
        cfg.tRRD_L = tm.get_num("tRRD_L", cfg.tRRD_L);
        cfg.tFAW = tm.get_num("tFAW", cfg.tFAW);
        cfg.tWTR_S = tm.get_num("tWTR_S", cfg.tWTR_S);
        cfg.tWTR_L = tm.get_num("tWTR_L", cfg.tWTR_L);
        cfg.tRTP = tm.get_num("tRTP", cfg.tRTP);
        cfg.tWR = tm.get_num("tWR", cfg.tWR);
        cfg.tREFI = tm.get_num("tREFI", cfg.tREFI);
        cfg.tRFC = tm.get_num("tRFC", cfg.tRFC);
        cfg.rd_wr_turnaround = tm.get_num("rd_wr_turnaround", cfg.rd_wr_turnaround);
        cfg.wr_rd_turnaround = tm.get_num("wr_rd_turnaround", cfg.wr_rd_turnaround);
    }

    if (root.contains("ddrc_resources")) {
        const json::Value& r = root["ddrc_resources"];
        cfg.command_queue_depth = static_cast<int>(r.get_int("command_queue_depth", cfg.command_queue_depth));
        cfg.max_outstanding_per_id = static_cast<int>(r.get_int("max_outstanding_per_id", cfg.max_outstanding_per_id));
        cfg.scheduling_policy = r.get_str("scheduling_policy", cfg.scheduling_policy);
    }

    if (root.contains("reporting")) {
        const json::Value& rp = root["reporting"];
        cfg.history_window_ns = rp.get_num("history_window_ns", cfg.history_window_ns);
    }

    cfg.validate();
    return cfg;
}

namespace {
bool is_power_of_two(long long v) { return v >= 1 && (v & (v - 1)) == 0; }
} // namespace

void DdrcConfig::validate() const {
    using std::to_string;

    // ---- topology: every count is a physical bus/array width, so it must
    // be a power of two (address bits gather-select it) and non-zero. ----
    auto check_count = [&](int value, const char* name) {
        if (!is_power_of_two(value)) {
            throw std::runtime_error("topology." + std::string(name) + " = " + to_string(value) +
                                      " must be a power of two >= 1");
        }
    };
    check_count(channels, "channels");
    check_count(ranks_per_channel, "ranks_per_channel");
    check_count(bankgroups, "bankgroups");
    check_count(banks_per_group, "banks_per_group");
    check_count(rows, "rows");
    check_count(columns, "columns");
    check_count(data_bus_bytes, "data_bus_bytes");
    check_count(burst_beats, "burst_beats");

    if (!(clock_mhz > 0.0)) {
        throw std::runtime_error("topology.clock_mhz = " + to_string(clock_mhz) + " must be > 0");
    }
    // clock_mhz is actually the effective data rate in MT/s, not the DRAM
    // core clock (see the comment on the field in config.hpp) -- the most
    // likely user error is entering the core clock instead (half the data
    // rate for DDR), or a raw Hz/GHz value by unit confusion. We can't tell
    // "core clock entered" from "a legitimately slow data rate" from one
    // number alone (e.g. 800 is both a valid DDR3-800 data rate and DDR4-1600's
    // core clock), so this only rejects values no real DDR/LPDDR generation
    // has ever shipped or roadmapped (roughly DDR-200 through current
    // DDR5/LPDDR5X parts), which are almost always a units mistake.
    if (clock_mhz < 100.0 || clock_mhz > 20000.0) {
        throw std::runtime_error(
            "topology.clock_mhz = " + to_string(clock_mhz) +
            " is outside any realistic DDR/LPDDR data rate (100-20000 MT/s); this field holds the "
            "effective data rate (e.g. 1600 for DDR4-1600), not the DRAM core clock -- if you entered "
            "the core clock, double it, since DDR transfers on both clock edges");
    }

    // ---- address mapping ----
    struct Named {
        const AddressField* field;
        const char* name;
    };
    const Named fields[] = {
        {&map_channel, "channel"}, {&map_rank, "rank"}, {&map_bankgroup, "bankgroup"},
        {&map_bank, "bank"},       {&map_row, "row"},
    };

    // Bit indices must land inside a uint64_t address.
    for (const auto& nf : fields) {
        for (int b : nf.field->bits) {
            if (b < 0 || b >= 64) {
                throw std::runtime_error("address_mapping." + std::string(nf.name) + ": bit index " +
                                          to_string(b) + " is out of range [0, 64)");
            }
        }
        for (int b : nf.field->hash_bits) {
            if (b < 0 || b >= 64) {
                throw std::runtime_error("address_mapping." + std::string(nf.name) + ".hash_bits: bit index " +
                                          to_string(b) + " is out of range [0, 64)");
            }
        }
    }

    // Field width must match its topology count: AddressField gathers one
    // physical bit per field bit, so a field can only ever address
    // 2^width() distinct values -- if that doesn't equal the topology count,
    // either some values are unreachable (width too small) or the field
    // claims bits the topology has no use for (width too large).
    auto check_width = [&](const AddressField& f, const char* name, int count, const char* count_name) {
        int expected_width = 0;
        while ((1 << expected_width) < count) ++expected_width;
        if (f.width() != expected_width) {
            throw std::runtime_error("address_mapping." + std::string(name) + ".width() = " +
                                      to_string(f.width()) + " does not match " + count_name + " = " +
                                      to_string(count) + " (needs width " + to_string(expected_width) +
                                      " so that 2^width == " + count_name + ")");
        }
    };
    check_width(map_channel, "channel", channels, "topology.channels");
    check_width(map_rank, "rank", ranks_per_channel, "topology.ranks_per_channel");
    check_width(map_bankgroup, "bankgroup", bankgroups, "topology.bankgroups");
    check_width(map_bank, "bank", banks_per_group, "topology.banks_per_group");

    // A physical bit driving two fields' *direct* bit-select (`bits`, the
    // gather list) can't be right: both fields would read the same wire, so
    // every address that differs only in that bit would move both fields in
    // lockstep -- e.g. bank and rank would be aliased for that bit, which is
    // not a real DDRC mapping.
    //
    // `hash_bits` sources are deliberately exempt from this check. XOR-hash
    // interleaving (AddressField::xor_hashed, see the class comment) exists
    // specifically to fold already-mapped high-order bits -- typically row
    // bits -- into the bank/bankgroup/channel selection, to break a
    // workload's accidental periodicity. A bit that feeds address_mapping.row
    // directly and *also* XOR-folds into address_mapping.bank's hash_bits is
    // the documented, intended use of that mechanism: the bit still
    // determines row directly, and additionally perturbs bank's selection --
    // it is not aliased, because bank's value isn't a bare copy of that bit,
    // it's XORed with other bits too. So only `bits` vs `bits` overlap (across
    // different fields) is treated as a conflict here.
    std::vector<std::pair<int, const char*>> claimed;
    for (const auto& nf : fields) {
        for (int b : nf.field->bits) {
            for (const auto& c : claimed) {
                if (c.first == b) {
                    throw std::runtime_error("address_mapping." + std::string(nf.name) +
                                              ": physical address bit " + to_string(b) +
                                              " is also used by address_mapping." + c.second +
                                              " (a physical bit may only feed one field's direct bit-select)");
                }
            }
            claimed.emplace_back(b, nf.name);
        }
    }

    // ---- timing: every value is a duration, so it must be non-negative,
    // and several pairs are definitional relationships where a violation
    // means a typo rather than an unusual-but-valid part. ----
    auto check_nonneg = [&](double v, const char* name) {
        if (v < 0.0) {
            throw std::runtime_error("timing_ns." + std::string(name) + " = " + to_string(v) + " must be >= 0");
        }
    };
    check_nonneg(tRCD, "tRCD");
    check_nonneg(tRP, "tRP");
    check_nonneg(tRAS, "tRAS");
    check_nonneg(tRC, "tRC");
    check_nonneg(tCCD_S, "tCCD_S");
    check_nonneg(tCCD_L, "tCCD_L");
    check_nonneg(tRRD_S, "tRRD_S");
    check_nonneg(tRRD_L, "tRRD_L");
    check_nonneg(tFAW, "tFAW");
    check_nonneg(tWTR_S, "tWTR_S");
    check_nonneg(tWTR_L, "tWTR_L");
    check_nonneg(tRTP, "tRTP");
    check_nonneg(tWR, "tWR");
    check_nonneg(tREFI, "tREFI");
    check_nonneg(tRFC, "tRFC");
    check_nonneg(rd_wr_turnaround, "rd_wr_turnaround");
    check_nonneg(wr_rd_turnaround, "wr_rd_turnaround");

    if (tRC < tRAS + tRP) {
        throw std::runtime_error("timing_ns.tRC = " + to_string(tRC) + " must be >= tRAS + tRP (tRAS = " +
                                  to_string(tRAS) + " + tRP = " + to_string(tRP) + " = " +
                                  to_string(tRAS + tRP) +
                                  "); tRC is defined as a full activate-to-activate cycle for one bank: the "
                                  "minimum row-active time (tRAS) followed by precharge (tRP)");
    }
    if (tCCD_L < tCCD_S) {
        throw std::runtime_error("timing_ns.tCCD_L = " + to_string(tCCD_L) + " must be >= tCCD_S = " +
                                  to_string(tCCD_S) +
                                  " (the same-bank-group column-to-column spacing can never be shorter than "
                                  "the different-bank-group spacing -- that's the reason bank groups exist)");
    }
    if (tRRD_L < tRRD_S) {
        throw std::runtime_error("timing_ns.tRRD_L = " + to_string(tRRD_L) + " must be >= tRRD_S = " +
                                  to_string(tRRD_S) +
                                  " (the same-bank-group activate-to-activate spacing can never be shorter "
                                  "than the different-bank-group spacing)");
    }
    if (tWTR_L < tWTR_S) {
        throw std::runtime_error("timing_ns.tWTR_L = " + to_string(tWTR_L) + " must be >= tWTR_S = " +
                                  to_string(tWTR_S) +
                                  " (the same-bank-group write-to-read spacing can never be shorter than the "
                                  "different-bank-group spacing)");
    }
    if (tFAW < 4.0 * tRRD_S) {
        throw std::runtime_error("timing_ns.tFAW = " + to_string(tFAW) + " must be >= 4 * tRRD_S (4 * " +
                                  to_string(tRRD_S) + " = " + to_string(4.0 * tRRD_S) +
                                  "); four activates spaced tRRD_S apart can't fit inside a shorter window, "
                                  "so a smaller tFAW would forbid the very spacing tRRD_S allows");
    }

    // ---- resources ----
    if (command_queue_depth < 1) {
        throw std::runtime_error("ddrc_resources.command_queue_depth = " + to_string(command_queue_depth) +
                                  " must be >= 1");
    }
    if (max_outstanding_per_id < 1) {
        throw std::runtime_error("ddrc_resources.max_outstanding_per_id = " + to_string(max_outstanding_per_id) +
                                  " must be >= 1");
    }
    // ChannelScheduler::pick_best_index/drain_one (command_queue.cpp) never
    // branch on scheduling_policy -- there is exactly one arbiter (FR-FCFS
    // priority: page-hit > idle bank > conflict, with starvation and
    // pagematch-streak mitigation), applied unconditionally. The header
    // comment on this field claims "fr_fcfs" | "in_order" are both
    // supported, but "in_order" has never been wired to any different
    // behavior (checked back to the initial commit) -- setting it silently
    // still runs FR-FCFS. Accepting it here would let a caller believe
    // they've selected strict in-order dispatch when they haven't, so only
    // the value that matches the actual implementation is allowed.
    if (scheduling_policy != "fr_fcfs") {
        throw std::runtime_error(
            "ddrc_resources.scheduling_policy = \"" + scheduling_policy +
            "\" is not implemented; ChannelScheduler (src/core/command_queue.cpp) always runs a single "
            "FR-FCFS-style arbiter regardless of this setting, so the only supported value is \"fr_fcfs\"");
    }

    // history_window_ns: 0 legitimately means "disabled" (see the field
    // comment), so only reject negative values.
    if (history_window_ns < 0.0) {
        throw std::runtime_error("reporting.history_window_ns = " + to_string(history_window_ns) +
                                  " must be >= 0 (0 disables windowed history accounting)");
    }
}

} // namespace ddrtiming

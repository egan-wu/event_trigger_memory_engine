#include "config.hpp"
#include "json.hpp"

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
        cfg.tCL = tm.get_num("tCL", cfg.tCL);
        cfg.tCWL = tm.get_num("tCWL", cfg.tCWL);
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

    return cfg;
}

} // namespace ddrtiming

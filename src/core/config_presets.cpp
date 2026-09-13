#include "config_presets.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ddrtiming {

namespace {

int width_for(int count) {
    int w = 0;
    while ((1 << w) < count) ++w;
    return w;
}

void set_default(json::Value& obj, const std::string& key, json::Value val) {
    if (!obj.contains(key)) obj.set(key, std::move(val));
}

// ---------------------------------------------------------------------
// DRAM presets: JEDEC-typical parameter sets, not exact spec values for any
// one vendor's part -- close enough for a 70-80%-accuracy estimator, and a
// starting point every field of which can still be overridden individually.
// ---------------------------------------------------------------------
struct DramPreset {
    const char* name;
    const char* source_note;
    int bankgroups, banks_per_group, rows, columns, data_bus_bytes, burst_beats;
    double clock_mhz;
    double tRCD, tRP, tRAS, tRC, tCL, tCWL;
    double tCCD_S, tCCD_L, tRRD_S, tRRD_L, tFAW;
    double tWTR_S, tWTR_L, tRTP, tWR;
    double tREFI, tRFC;
    double rd_wr_turnaround, wr_rd_turnaround;
};

const DramPreset kDramPresets[] = {
    {"DDR4-2400", "JEDEC JESD79-4 typical timings, CL16-16-16 grade, 8Gb density (tRFC).",
     4, 4, 65536, 1024, 8, 8, 2400.0,
     13.32, 13.32, 32.0, 45.32, 13.32, 10.0,
     3.33, 5.0, 3.3, 4.9, 21.0,
     2.5, 7.5, 7.5, 15.0,
     7800.0, 350.0, 2.5, 2.5},
    {"DDR4-3200", "JEDEC JESD79-4 typical timings, CL22-22-22 grade, 8Gb density; matches examples/ddrc_config.example.json.",
     4, 4, 65536, 1024, 8, 8, 3200.0,
     13.75, 13.75, 32.0, 45.75, 13.75, 12.5,
     2.5, 5.0, 2.5, 4.9, 21.0,
     2.5, 7.5, 7.5, 15.0,
     7800.0, 350.0, 2.5, 2.5},
    {"DDR5-4800", "JEDEC JESD79-5 typical timings, CL40 grade, 16Gb density. This tool does not model DDR5's two "
                  "32-bit sub-channels separately -- treated here as one 64-bit channel (data_bus_bytes=8).",
     8, 4, 65536, 1024, 8, 16, 4800.0,
     16.0, 16.0, 32.0, 48.0, 16.67, 15.0,
     3.33, 5.0, 3.33, 5.0, 13.33,
     2.5, 10.0, 7.5, 30.0,
     3900.0, 295.0, 2.5, 2.5},
    {"LPDDR4-4266", "JEDEC JESD209-4 typical timings, 6Gb density. No bank groups (bankgroups=1); BL16 burst.",
     1, 8, 65536, 1024, 4, 16, 4266.0,
     18.0, 18.0, 42.0, 60.0, 16.0, 8.0,
     3.75, 3.75, 7.5, 7.5, 30.0,
     10.0, 10.0, 7.5, 18.0,
     3904.0, 280.0, 2.5, 2.5},
    {"LPDDR5-6400", "JEDEC JESD209-5 typical timings, bank-group mode, 12Gb density; data_bus_bytes=2 models one "
                    "16-bit channel.",
     4, 4, 65536, 1024, 2, 16, 6400.0,
     18.0, 18.0, 42.0, 60.0, 17.0, 9.0,
     2.5, 5.0, 2.5, 5.0, 20.0,
     2.5, 10.0, 7.5, 18.0,
     3904.0, 210.0, 2.5, 2.5},
};

const DramPreset* find_dram_preset(const std::string& name) {
    for (const auto& p : kDramPresets) {
        if (name == p.name) return &p;
    }
    return nullptr;
}

std::string list_dram_preset_names() {
    std::ostringstream os;
    for (size_t i = 0; i < sizeof(kDramPresets) / sizeof(kDramPresets[0]); ++i) {
        if (i) os << ", ";
        os << kDramPresets[i].name;
    }
    return os.str();
}

json::Value expand_dram_preset(json::Value root) {
    if (!root.contains("dram")) return root;
    const json::Value dram = root["dram"];
    std::string preset_name = dram.get_str("preset", "");
    if (preset_name.empty()) {
        throw std::runtime_error("\"dram\" object present but missing its required \"preset\" field");
    }
    const DramPreset* p = find_dram_preset(preset_name);
    if (!p) {
        throw std::runtime_error("unknown dram preset \"" + preset_name + "\"; valid presets: " +
                                  list_dram_preset_names());
    }

    if (!root.contains("topology")) root.set("topology", json::Value::make_object());
    json::Value topology = root["topology"];
    set_default(topology, "bankgroups", p->bankgroups);
    set_default(topology, "banks_per_group", p->banks_per_group);
    set_default(topology, "rows", p->rows);
    set_default(topology, "columns", p->columns);
    set_default(topology, "data_bus_bytes", p->data_bus_bytes);
    set_default(topology, "burst_beats", p->burst_beats);
    set_default(topology, "clock_mhz", p->clock_mhz);
    // Convenience: any other key on "dram" itself (e.g. "channels": 2) is a
    // topology override, so a preset user doesn't need a whole separate
    // "topology" block just to add one field.
    for (const auto& kv : dram.object_items()) {
        if (kv.first == "preset") continue;
        set_default(topology, kv.first, kv.second);
    }
    root.set("topology", topology);

    if (!root.contains("timing_ns")) root.set("timing_ns", json::Value::make_object());
    json::Value timing = root["timing_ns"];
    set_default(timing, "tRCD", p->tRCD);
    set_default(timing, "tRP", p->tRP);
    set_default(timing, "tRAS", p->tRAS);
    set_default(timing, "tRC", p->tRC);
    set_default(timing, "tCL", p->tCL);
    set_default(timing, "tCWL", p->tCWL);
    set_default(timing, "tCCD_S", p->tCCD_S);
    set_default(timing, "tCCD_L", p->tCCD_L);
    set_default(timing, "tRRD_S", p->tRRD_S);
    set_default(timing, "tRRD_L", p->tRRD_L);
    set_default(timing, "tFAW", p->tFAW);
    set_default(timing, "tWTR_S", p->tWTR_S);
    set_default(timing, "tWTR_L", p->tWTR_L);
    set_default(timing, "tRTP", p->tRTP);
    set_default(timing, "tWR", p->tWR);
    set_default(timing, "tREFI", p->tREFI);
    set_default(timing, "tRFC", p->tRFC);
    set_default(timing, "rd_wr_turnaround", p->rd_wr_turnaround);
    set_default(timing, "wr_rd_turnaround", p->wr_rd_turnaround);
    root.set("timing_ns", timing);

    return root;
}

// ---------------------------------------------------------------------
// Address-mapping presets. All three place fields contiguously starting at
// the burst offset (the low bits below any of them, implicitly the
// column/byte offset within one DRAM burst) and finish with `row` as the
// widest, highest field. `col_gap` -- extra unmapped bits representing a
// column address wider than one burst -- is inserted at the position noted
// for each preset; it is a fixed span (columns' own bit width minus the
// burst's own contribution to it), not "whatever's left over", so it does
// not depend on which fields happen to precede it.
// ---------------------------------------------------------------------

json::Value bit_field(int bit_start, int width) {
    json::Value f = json::Value::make_object();
    f.set("bit_start", bit_start);
    f.set("bit_width", width);
    return f;
}

json::Value expand_address_mapping_preset(json::Value root) {
    if (!root.contains("address_mapping")) return root;
    const json::Value am = root["address_mapping"];
    if (!am.contains("preset")) return root; // no preset requested -- nothing to expand
    std::string preset_name = am.get_str("preset", "");

    const json::Value& topo = root["topology"];
    int channels = static_cast<int>(topo.get_int("channels", 1));
    int ranks = static_cast<int>(topo.get_int("ranks_per_channel", 1));
    int bankgroups = static_cast<int>(topo.get_int("bankgroups", 1));
    int banks_per_group = static_cast<int>(topo.get_int("banks_per_group", 4));
    int columns = static_cast<int>(topo.get_int("columns", 1024));
    int data_bus_bytes = static_cast<int>(topo.get_int("data_bus_bytes", 8));
    int burst_beats = static_cast<int>(topo.get_int("burst_beats", 8));
    int rows = static_cast<int>(topo.get_int("rows", 65536));

    int burst_offset = width_for(std::max(1, data_bus_bytes) * std::max(1, burst_beats));
    int col_gap = std::max(0, width_for(std::max(1, columns)) - width_for(std::max(1, burst_beats)));

    int w_channel = width_for(std::max(1, channels));
    int w_rank = width_for(std::max(1, ranks));
    int w_bankgroup = width_for(std::max(1, bankgroups));
    int w_bank = width_for(std::max(1, banks_per_group));
    int w_row = width_for(std::max(1, rows));

    // Each preset names the order fields are laid out in, and where col_gap
    // sits in that order (as its own pseudo-entry). Widths of 0 (a
    // single-valued field, e.g. channels=1) contribute no bits and are
    // effectively skipped.
    struct Entry {
        const char* field; // "channel"/"rank"/"bankgroup"/"bank"/"gap"
        int width;
    };
    std::vector<Entry> order;
    if (preset_name == "bankgroup-fast") {
        order = {{"bankgroup", w_bankgroup}, {"bank", w_bank}, {"channel", w_channel},
                 {"rank", w_rank}, {"gap", col_gap}};
    } else if (preset_name == "channel-low") {
        order = {{"channel", w_channel}, {"bankgroup", w_bankgroup}, {"bank", w_bank},
                 {"rank", w_rank}, {"gap", col_gap}};
    } else if (preset_name == "bank-per-core") {
        order = {{"bankgroup", w_bankgroup}, {"channel", w_channel}, {"gap", col_gap},
                 {"bank", w_bank}, {"rank", w_rank}};
    } else {
        throw std::runtime_error("unknown address_mapping preset \"" + preset_name +
                                  "\"; valid presets: bankgroup-fast, channel-low, bank-per-core");
    }

    int cursor = burst_offset;
    json::Value expanded = json::Value::make_object();
    for (const auto& e : order) {
        if (std::string(e.field) != "gap") expanded.set(e.field, bit_field(cursor, e.width));
        cursor += e.width;
    }
    expanded.set("row", bit_field(cursor, w_row));

    // Explicit per-field entries the caller already wrote win over the
    // preset for that field only (e.g. keep the preset's bankgroup/bank/
    // channel but hand-place "row" -- or vice versa).
    json::Value final_am = json::Value::make_object();
    for (const char* field : {"channel", "rank", "bankgroup", "bank", "row"}) {
        if (am.contains(field)) {
            final_am.set(field, am[field]);
        } else if (expanded.contains(field)) {
            final_am.set(field, expanded[field]);
        }
    }
    root.set("address_mapping", final_am);
    return root;
}

} // namespace

json::Value expand_presets(json::Value root) {
    root = expand_dram_preset(std::move(root));
    root = expand_address_mapping_preset(std::move(root));
    return root;
}

} // namespace ddrtiming

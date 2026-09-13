// [C] Config-layer presets (src/core/config_presets.cpp) and their
// integration into DdrcConfig::load_from_file()/validate(), plus the
// sha256 implementation behind report provenance (src/core/sha256.cpp).
#include "testing.hpp"
#include "core/config.hpp"
#include "core/config_presets.hpp"
#include "core/json.hpp"
#include "core/sha256.hpp"

#include <cstdio>
#include <fstream>

using namespace ddrtiming;

namespace {
// Writes `text` to a temp file and returns its path, so DdrcConfig::
// load_from_file() (which only takes a path) can be exercised the same way
// a real caller would use it, matching test_concurrency.cpp's pattern.
std::string write_temp_config(const std::string& name, const std::string& text) {
    std::ofstream f(name, std::ios::binary);
    f << text;
    f.close();
    return name;
}
} // namespace

DDRTEST(dram_preset_fills_topology_and_timing) {
    json::Value root = json::parse(R"({"dram": {"preset": "DDR4-3200"}})");
    root = expand_presets(std::move(root));
    DDR_CHECK_EQ(root["topology"].get_int("bankgroups", -1), 4);
    DDR_CHECK_EQ(root["topology"].get_int("banks_per_group", -1), 4);
    DDR_CHECK_EQ(root["topology"].get_int("data_bus_bytes", -1), 8);
    DDR_CHECK(root["topology"].get_num("clock_mhz", -1.0) == 3200.0);
    DDR_CHECK(root["timing_ns"].get_num("tRCD", -1.0) == 13.75);
    DDR_CHECK(root["timing_ns"].get_num("tCCD_L", -1.0) == 5.0);
    DDR_CHECK(root["timing_ns"].get_num("tRFC", -1.0) == 350.0);
}

DDRTEST(dram_preset_convenience_key_and_explicit_field_both_work) {
    // "channels" on the dram object itself is a topology convenience
    // shortcut; an explicit sibling "topology" field wins over the preset.
    json::Value root = json::parse(
        R"({"dram": {"preset": "DDR4-3200", "channels": 2}, "topology": {"data_bus_bytes": 16}})");
    root = expand_presets(std::move(root));
    DDR_CHECK_EQ(root["topology"].get_int("channels", -1), 2);
    DDR_CHECK_EQ(root["topology"].get_int("data_bus_bytes", -1), 16); // explicit beats the preset's 8
}

DDRTEST(unknown_dram_preset_throws_and_lists_valid_names) {
    json::Value root = json::parse(R"({"dram": {"preset": "DDR9-9999"}})");
    bool threw = false;
    try {
        expand_presets(std::move(root));
    } catch (const std::exception& e) {
        threw = true;
        std::string msg = e.what();
        DDR_CHECK(msg.find("DDR9-9999") != std::string::npos);
        DDR_CHECK(msg.find("DDR4-3200") != std::string::npos); // one real name, as a sanity check on the list
    }
    DDR_CHECK(threw);
}

// Hand-computed: 1 channel (width 0), 4 bankgroups (width 2), 4 banks/group
// (width 2), columns=1024 (width 10), burst_beats=8 (width 3) ->
// burst_offset = width_for(data_bus_bytes(8)*burst_beats(8)=64) = 6.
// bankgroup-fast order is [bankgroup, bank, channel, rank, gap]:
//   bankgroup @6 w2 -> next 8; bank @8 w2 -> next 10;
//   channel @10 w0 (1 channel) -> next 10; rank @10 w0 -> next 10;
//   gap = width_for(columns=1024)=10 - width_for(burst_beats=8)=3 = 7 -> next 17.
//   row @17, width_for(rows=65536) = 16.
DDRTEST(bankgroup_fast_address_preset_matches_hand_computed_layout) {
    json::Value root = json::parse(R"({
      "topology": {"channels": 1, "bankgroups": 4, "banks_per_group": 4,
                    "rows": 65536, "columns": 1024, "data_bus_bytes": 8, "burst_beats": 8},
      "address_mapping": {"preset": "bankgroup-fast"}
    })");
    root = expand_presets(std::move(root));
    const json::Value& am = root["address_mapping"];
    DDR_CHECK_EQ(am["bankgroup"].get_int("bit_start", -1), 6);
    DDR_CHECK_EQ(am["bankgroup"].get_int("bit_width", -1), 2);
    DDR_CHECK_EQ(am["bank"].get_int("bit_start", -1), 8);
    DDR_CHECK_EQ(am["bank"].get_int("bit_width", -1), 2);
    DDR_CHECK_EQ(am["row"].get_int("bit_start", -1), 17);
    DDR_CHECK_EQ(am["row"].get_int("bit_width", -1), 16);

    // The whole point of this preset: it must actually validate (widths
    // matching topology counts, no bit overlaps) when run through the real
    // DdrcConfig loader, not just look right by eye.
    root.set("ddrc_resources", json::Value::make_object());
    std::ofstream f("preset_bankgroup_fast_test.json", std::ios::binary);
    f << root.dump(2);
    f.close();
    DdrcConfig cfg = DdrcConfig::load_from_file("preset_bankgroup_fast_test.json"); // throws on failure
    (void)cfg;
    std::remove("preset_bankgroup_fast_test.json");
}

DDRTEST(address_mapping_explicit_field_overrides_preset) {
    json::Value root = json::parse(R"({
      "topology": {"channels": 1, "bankgroups": 4, "banks_per_group": 4,
                    "rows": 65536, "columns": 1024, "data_bus_bytes": 8, "burst_beats": 8},
      "address_mapping": {"preset": "bankgroup-fast", "row": {"bit_start": 20, "bit_width": 16}}
    })");
    root = expand_presets(std::move(root));
    const json::Value& am = root["address_mapping"];
    DDR_CHECK_EQ(am["row"].get_int("bit_start", -1), 20); // explicit, not the preset's 17
    DDR_CHECK_EQ(am["bankgroup"].get_int("bit_start", -1), 6); // preset still fills the untouched fields
}

DDRTEST(unknown_address_mapping_preset_throws) {
    json::Value root = json::parse(
        R"({"topology": {"channels": 1}, "address_mapping": {"preset": "not-a-real-preset"}})");
    bool threw = false;
    try {
        expand_presets(std::move(root));
    } catch (const std::exception&) {
        threw = true;
    }
    DDR_CHECK(threw);
}

DDRTEST(unset_trc_derives_from_tras_plus_trp) {
    std::string path = write_temp_config("trc_derive_test.json", R"({
      "topology": {"channels": 1, "bankgroups": 1, "banks_per_group": 1, "rows": 1, "columns": 1,
                    "data_bus_bytes": 8, "clock_mhz": 1600},
      "timing_ns": {"tRAS": 30.0, "tRP": 10.0},
      "ddrc_resources": {}
    })");
    DdrcConfig cfg = DdrcConfig::load_from_file(path);
    DDR_CHECK(cfg.tRC == 40.0); // 30 + 10, not the struct default 45.75
    std::remove(path.c_str());
}

DDRTEST(explicit_trc_is_kept_when_consistent) {
    std::string path = write_temp_config("trc_explicit_test.json", R"({
      "topology": {"channels": 1, "bankgroups": 1, "banks_per_group": 1, "rows": 1, "columns": 1,
                    "data_bus_bytes": 8, "clock_mhz": 1600},
      "timing_ns": {"tRAS": 30.0, "tRP": 10.0, "tRC": 41.0},
      "ddrc_resources": {}
    })");
    DdrcConfig cfg = DdrcConfig::load_from_file(path);
    DDR_CHECK(cfg.tRC == 41.0); // explicit value kept as-is (still >= tRAS+tRP, so validate() accepts it)
    std::remove(path.c_str());
}

DDRTEST(map_row_width_mismatch_is_caught) {
    DdrcConfig cfg;
    cfg.channels = 1; cfg.ranks_per_channel = 1; cfg.bankgroups = 1; cfg.banks_per_group = 1;
    cfg.rows = 1 << 16; // needs a 16-bit row field
    cfg.map_row = AddressField::contiguous(11, 10); // only 10 bits -- mismatch
    bool threw = false;
    try {
        cfg.validate();
    } catch (const std::exception& e) {
        threw = true;
        std::string msg = e.what();
        DDR_CHECK(msg.find("row") != std::string::npos);
    }
    DDR_CHECK(threw);
}

DDRTEST(sha256_matches_known_test_vectors) {
    // Cross-checked against Python's hashlib.sha256() at authoring time
    // (both the standard "" / "abc" vectors and a real file's contents),
    // not just copied from memory -- see the commit message.
    DDR_CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    DDR_CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

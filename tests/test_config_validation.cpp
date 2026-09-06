#include "testing.hpp"
#include "core/config.hpp"

#include <stdexcept>
#include <string>

using namespace ddrtiming;

namespace {

// Mirrors examples/ddrc_config.example.json: a single channel/rank, 4
// bankgroups x 4 banks, and the shipped timing_ns block (which is already
// self-consistent, so most fields can be left at their struct defaults).
DdrcConfig valid_config() {
    DdrcConfig cfg;
    cfg.channels = 1;
    cfg.ranks_per_channel = 1;
    cfg.bankgroups = 4;
    cfg.banks_per_group = 4;
    cfg.rows = 1 << 16;
    cfg.columns = 1 << 10;
    cfg.data_bus_bytes = 8;
    cfg.burst_beats = 8;
    cfg.clock_mhz = 1600.0;

    // map_channel / map_rank stay default-empty (width 0), matching
    // channels == ranks_per_channel == 1.
    cfg.map_row = AddressField::contiguous(11, 16);
    cfg.map_bankgroup = AddressField::contiguous(27, 2);
    cfg.map_bank = AddressField::contiguous(29, 2);

    cfg.scheduling_policy = "fr_fcfs";
    cfg.command_queue_depth = 32;
    cfg.max_outstanding_per_id = 8;
    cfg.history_window_ns = 0.0;
    return cfg;
}

// Runs fn() and returns the message of the std::exception it throws.
// Fails the test (via TestFailure) if fn() does not throw.
template <typename Fn>
std::string expect_throw(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    throw TestFailure("expected validate() to throw std::runtime_error, but it did not");
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

DDRTEST(valid_config_passes_validation) {
    DdrcConfig cfg = valid_config();
    try {
        cfg.validate();
    } catch (const std::exception& e) {
        throw TestFailure(std::string("expected a valid config to pass validate(), but it threw: ") + e.what());
    }
}

DDRTEST(address_bit_overlap_is_caught) {
    DdrcConfig cfg = valid_config();
    // map_row occupies bits [11,27). Make map_bank steal one of those bits
    // (11) via its direct gather list -- a real conflict, since both fields
    // would then read the same physical wire.
    cfg.map_bank = AddressField::contiguous(11, 2);

    std::string msg = expect_throw([&] { cfg.validate(); });
    DDR_CHECK(contains(msg, "address_mapping.bank"));
    DDR_CHECK(contains(msg, "address_mapping.row"));
    DDR_CHECK(contains(msg, "11"));
}

DDRTEST(address_field_width_topology_mismatch_is_caught) {
    DdrcConfig cfg = valid_config();
    // bankgroups = 8 needs a 3-bit field, but map_bankgroup is left at its
    // 2-bit mapping from valid_config().
    cfg.bankgroups = 8;

    std::string msg = expect_throw([&] { cfg.validate(); });
    DDR_CHECK(contains(msg, "address_mapping.bankgroup"));
    DDR_CHECK(contains(msg, "topology.bankgroups"));
    DDR_CHECK(contains(msg, "8"));
}

DDRTEST(non_power_of_two_topology_count_is_caught) {
    DdrcConfig cfg = valid_config();
    cfg.channels = 3;

    std::string msg = expect_throw([&] { cfg.validate(); });
    DDR_CHECK(contains(msg, "topology.channels"));
    DDR_CHECK(contains(msg, "3"));
}

DDRTEST(trc_less_than_tras_plus_trp_is_caught) {
    DdrcConfig cfg = valid_config();
    cfg.tRAS = 32.0;
    cfg.tRP = 13.75;
    cfg.tRC = 10.0; // well under tRAS + tRP == 45.75

    std::string msg = expect_throw([&] { cfg.validate(); });
    DDR_CHECK(contains(msg, "tRC"));
    DDR_CHECK(contains(msg, "tRAS"));
    DDR_CHECK(contains(msg, "tRP"));
}

DDRTEST(tccd_l_less_than_tccd_s_is_caught) {
    DdrcConfig cfg = valid_config();
    cfg.tCCD_S = 4.0;
    cfg.tCCD_L = 2.0; // same-bank-group spacing can't be faster

    std::string msg = expect_throw([&] { cfg.validate(); });
    DDR_CHECK(contains(msg, "tCCD_L"));
    DDR_CHECK(contains(msg, "tCCD_S"));
}

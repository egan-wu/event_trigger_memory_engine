#pragma once
#include "json.hpp"

namespace ddrtiming {

// Expands preset shorthand on a freshly-parsed (but not yet DdrcConfig-typed)
// config JSON tree, in place conceptually (returns the expanded tree; the
// input may be moved from). Two independent presets, applied in this order
// (address-mapping needs the topology counts, which the dram preset may
// itself have just filled in):
//
//   "dram": {"preset": "DDR4-3200", ...}
//     Fills topology.{bankgroups,banks_per_group,rows,columns,
//     data_bus_bytes,burst_beats,clock_mhz} and every timing_ns.* field from
//     a named, JEDEC-typical parameter set (see config_presets.cpp for the
//     table and per-preset source notes). channels/ranks_per_channel are
//     deliberately NOT part of any preset (topology, not DRAM-device,
//     properties) -- default to 1 unless given. Any key already present in
//     "topology"/"timing_ns" wins over the preset; the "dram" object itself
//     may also carry convenience topology keys (e.g. "channels": 2) copied
//     into "topology" under the same rule.
//
//   "address_mapping": {"preset": "bankgroup-fast" | "channel-low" |
//                        "bank-per-core", ...per-field overrides}
//     Expands to {"bit_start","bit_width"} objects for channel/rank/
//     bankgroup/bank/row, computed from the (by now expanded) topology --
//     see config_presets.cpp for the exact bit layout each name produces.
//     An explicit object already present for a given field (e.g. a
//     hand-written "row") wins over the preset for that field only.
//
// A config with neither top-level key is returned byte-for-byte equivalent
// to the input (no expansion performed). Throws std::runtime_error (naming
// the bad value and, for an unknown preset, every valid name) on a
// "preset" key whose value isn't recognized.
json::Value expand_presets(json::Value root);

} // namespace ddrtiming

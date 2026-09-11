#pragma once
#include "config.hpp"
#include "types.hpp"

namespace ddrtiming {

// Decodes a physical address into {channel, rank, bankgroup, bank, row, col}
// using the contiguous bit-field mapping described in DdrcConfig. Any field
// left unconfigured (bit_width == 0) resolves to 0. "col" is derived from the
// remaining low-order bits not claimed by channel/bankgroup/bank (row is
// typically the high bits and excluded from column derivation on purpose,
// since row address doesn't factor into intra-row column offset).
class AddressDecoder {
public:
    explicit AddressDecoder(const DdrcConfig& cfg) : cfg_(cfg) {}

    DecodedAddr decode(uint64_t addr) const;

    // Number of low-order address bits this mapping actually decodes: one
    // past the highest physical bit any field reads, gather or hash (0 if
    // nothing is mapped). Two addresses that differ only at or above this
    // bit decode to the very same channel/rank/bankgroup/bank/row/column --
    // the mapping has no way to tell them apart, so they alias.
    int mapped_address_bits() const;

private:
    static uint64_t extract(uint64_t addr, const AddressField& f);
    const DdrcConfig& cfg_;
};

} // namespace ddrtiming

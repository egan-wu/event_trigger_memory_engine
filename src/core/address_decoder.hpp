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

private:
    static uint64_t extract(uint64_t addr, const AddressField& f);
    const DdrcConfig& cfg_;
};

} // namespace ddrtiming

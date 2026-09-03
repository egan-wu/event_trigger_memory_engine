#include "address_decoder.hpp"

#include <algorithm>

namespace ddrtiming {

uint64_t AddressDecoder::extract(uint64_t addr, const AddressField& f) {
    uint64_t v = 0;
    for (size_t i = 0; i < f.bits.size(); ++i) {
        int b = f.bits[i];
        if (b < 0 || b >= 64) continue;
        uint64_t bitval = (addr >> b) & 1ull;
        v |= (bitval << i);
    }
    return v;
}

DecodedAddr AddressDecoder::decode(uint64_t addr) const {
    DecodedAddr d;
    d.channel = static_cast<uint32_t>(extract(addr, cfg_.map_channel));
    d.rank = static_cast<uint32_t>(extract(addr, cfg_.map_rank));
    d.bankgroup = static_cast<uint32_t>(extract(addr, cfg_.map_bankgroup));
    d.bank = static_cast<uint32_t>(extract(addr, cfg_.map_bank));
    d.row = static_cast<uint32_t>(extract(addr, cfg_.map_row));

    // Column/byte-offset: informational only (doesn't drive scheduling). With
    // scattered mappings there's no single clean boundary, so approximate it
    // as everything below the lowest bit claimed by bank (or bankgroup).
    int col_width = 0;
    if (!cfg_.map_bank.bits.empty()) {
        col_width = *std::min_element(cfg_.map_bank.bits.begin(), cfg_.map_bank.bits.end());
    } else if (!cfg_.map_bankgroup.bits.empty()) {
        col_width = *std::min_element(cfg_.map_bankgroup.bits.begin(), cfg_.map_bankgroup.bits.end());
    }
    if (col_width > 0 && col_width < 63) {
        d.col = addr & ((1ull << col_width) - 1);
    } else {
        d.col = 0;
    }
    return d;
}

} // namespace ddrtiming

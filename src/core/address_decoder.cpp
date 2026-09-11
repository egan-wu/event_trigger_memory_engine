#include "address_decoder.hpp"

#include <algorithm>

namespace ddrtiming {

uint64_t AddressDecoder::extract(uint64_t addr, const AddressField& f) {
    uint64_t v = 0;
    for (size_t i = 0; i < f.bits.size(); ++i) {
        int b = f.bits[i];
        uint64_t bitval = (b >= 0 && b < 64) ? (addr >> b) & 1ull : 0;
        if (i < f.hash_bits.size()) {
            int hb = f.hash_bits[i];
            if (hb >= 0 && hb < 64) bitval ^= (addr >> hb) & 1ull;
        }
        v |= (bitval << i);
    }
    return v;
}

int AddressDecoder::mapped_address_bits() const {
    int highest = -1;
    for (const AddressField* f : {&cfg_.map_channel, &cfg_.map_rank, &cfg_.map_bankgroup, &cfg_.map_bank, &cfg_.map_row}) {
        for (int b : f->bits) highest = std::max(highest, b);
        for (int b : f->hash_bits) highest = std::max(highest, b);
    }
    return highest + 1;
}

DecodedAddr AddressDecoder::decode(uint64_t addr) const {
    DecodedAddr d;
    d.channel = static_cast<uint32_t>(extract(addr, cfg_.map_channel));
    d.rank = static_cast<uint32_t>(extract(addr, cfg_.map_rank));
    d.bankgroup = static_cast<uint32_t>(extract(addr, cfg_.map_bankgroup));
    d.bank = static_cast<uint32_t>(extract(addr, cfg_.map_bank));
    d.row = static_cast<uint32_t>(extract(addr, cfg_.map_row));

    // Column/byte-offset: informational only (doesn't drive scheduling). With
    // scattered/hashed mappings there's no single clean boundary, so
    // approximate it as everything below the lowest bit claimed by bank (or
    // bankgroup), across both its gather bits and any hash bits.
    auto lowest_bit = [](const AddressField& f) -> int {
        int m = 64;
        bool any = false;
        for (int b : f.bits) if (b >= 0) { m = std::min(m, b); any = true; }
        for (int b : f.hash_bits) if (b >= 0) { m = std::min(m, b); any = true; }
        return any ? m : -1;
    };
    int col_width = 0;
    if (!cfg_.map_bank.bits.empty()) {
        col_width = lowest_bit(cfg_.map_bank);
    } else if (!cfg_.map_bankgroup.bits.empty()) {
        col_width = lowest_bit(cfg_.map_bankgroup);
    }
    if (col_width > 0 && col_width < 63) {
        d.col = addr & ((1ull << col_width) - 1);
    } else {
        d.col = 0;
    }
    return d;
}

} // namespace ddrtiming

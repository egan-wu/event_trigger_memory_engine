#include "testing.hpp"
#include "core/address_decoder.hpp"
#include "core/config.hpp"

using namespace ddrtiming;

DDRTEST(decode_contiguous_fields) {
    DdrcConfig cfg;
    cfg.map_channel = AddressField::contiguous(5, 1);
    cfg.map_bankgroup = AddressField::contiguous(6, 2);
    cfg.map_bank = AddressField::contiguous(8, 2);
    cfg.map_row = AddressField::contiguous(17, 16);
    cfg.map_rank = AddressField::contiguous(33, 1);

    AddressDecoder dec(cfg);

    uint64_t addr = 0;
    addr |= (1ull << 5);      // channel = 1
    addr |= (2ull << 6);      // bankgroup = 2
    addr |= (3ull << 8);      // bank = 3
    addr |= (1234ull << 17);  // row = 1234
    addr |= (1ull << 33);     // rank = 1

    DecodedAddr d = dec.decode(addr);
    DDR_CHECK_EQ(d.channel, 1u);
    DDR_CHECK_EQ(d.bankgroup, 2u);
    DDR_CHECK_EQ(d.bank, 3u);
    DDR_CHECK_EQ(d.row, 1234u);
    DDR_CHECK_EQ(d.rank, 1u);
}

DDRTEST(decode_scattered_bank_bits) {
    // bank[1:0] <- addr[15:14], bank[3:2] <- addr[11:10]
    DdrcConfig cfg;
    cfg.map_bank.bits = {14, 15, 10, 11};

    AddressDecoder dec(cfg);

    uint64_t addr = 0;
    addr |= (1ull << 14); // bank bit0 = 1
    addr |= (0ull << 15); // bank bit1 = 0
    addr |= (1ull << 10); // bank bit2 = 1
    addr |= (1ull << 11); // bank bit3 = 1
    // expected bank = bit3 bit2 bit1 bit0 = 1 1 0 1 = 0b1101 = 13
    DecodedAddr d = dec.decode(addr);
    DDR_CHECK_EQ(d.bank, 13u);
}

DDRTEST(decode_unconfigured_fields_are_zero) {
    DdrcConfig cfg; // default map_* all empty
    AddressDecoder dec(cfg);
    DecodedAddr d = dec.decode(0xdeadbeefull);
    DDR_CHECK_EQ(d.channel, 0u);
    DDR_CHECK_EQ(d.rank, 0u);
    DDR_CHECK_EQ(d.bankgroup, 0u);
    DDR_CHECK_EQ(d.bank, 0u);
    DDR_CHECK_EQ(d.row, 0u);
}

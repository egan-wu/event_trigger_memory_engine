#include "log_parser.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ddrtiming {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') { out.push_back(trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(trim(cur));
    return out;
}

uint64_t parse_uint(const std::string& s, uint64_t def = 0) {
    if (s.empty()) return def;
    bool hex = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
    return std::strtoull(s.c_str(), nullptr, hex ? 16 : 10);
}

std::vector<uint8_t> parse_hex_bytes(const std::string& s) {
    std::vector<uint8_t> out;
    std::string h = s;
    if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.empty()) return out;
    if (h.size() % 2 != 0) h = "0" + h;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
    }
    return out;
}

} // namespace

std::vector<LogEntry> parse_axi_log_file(const std::string& path, int core_id) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open AXI log file: " + path);

    std::vector<LogEntry> out;
    std::string line;
    bool header_skipped = false;
    size_t line_no = 0;

    while (std::getline(f, line)) {
        ++line_no;
        std::string t = trim(line);
        if (t.empty()) continue;
        if (!header_skipped) { header_skipped = true; continue; }

        std::vector<std::string> cols = split_csv(t);
        if (cols.empty()) continue;

        std::string type_str = cols[0];
        for (char& c : type_str) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        if (type_str == "BARRIER") {
            LogEntry entry;
            entry.is_barrier = true;
            out.push_back(std::move(entry));
            continue;
        }

        if (cols.size() < 5) {
            throw std::runtime_error("malformed AXI log line " + std::to_string(line_no) +
                                      " in " + path + ": expected at least 5 columns");
        }

        LogEntry entry;
        AxiTxn& txn = entry.txn;
        txn.core_id = core_id;
        if (type_str == "AR") txn.type = TxnType::Read;
        else if (type_str == "AW") txn.type = TxnType::Write;
        else throw std::runtime_error("malformed AXI log line " + std::to_string(line_no) +
                                       " in " + path + ": type must be AR, AW, or BARRIER, got '" + cols[0] + "'");

        txn.axi_id = static_cast<uint32_t>(parse_uint(cols[1], 0));
        txn.addr = parse_uint(cols[2], 0);
        txn.size_bytes = static_cast<uint32_t>(parse_uint(cols[3], 0));
        txn.len_beats = static_cast<uint32_t>(parse_uint(cols[4], 1));
        if (cols.size() >= 6 && !cols[5].empty()) {
            txn.wstrb = parse_hex_bytes(cols[5]);
        }

        if (txn.size_bytes == 0) {
            throw std::runtime_error("malformed AXI log line " + std::to_string(line_no) +
                                      " in " + path + ": size must be > 0");
        }

        out.push_back(std::move(entry));
    }

    return out;
}

} // namespace ddrtiming

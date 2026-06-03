#pragma once
#include <functional>
#include <string>
#include <vector>

namespace cortex::serving {
// Lightweight row abstraction
struct DbRow {
    std::vector<std::string> columns;
    std::string get(size_t idx) const { return idx < columns.size() ? columns[idx] : ""; }
};

class IDatabase {
public:
    virtual ~IDatabase() = default;
    virtual std::vector<DbRow> query(const std::string& sql) = 0;
    virtual bool connected() const = 0;
};
} // namespace cortex::serving

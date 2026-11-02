#include "cpm/common/csv_log.hpp"

namespace cpm {

std::string csv_field(std::string_view text) {
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(text);
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('"');
    for (const char c : text) {
        if (c == '"') quoted.push_back('"');
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

std::string csv_row(const std::vector<std::string>& fields) {
    std::string row;
    for (const auto& field : fields) {
        if (!row.empty()) row.push_back(',');
        row += csv_field(field);
    }
    return row;
}

CsvLog::CsvLog(DailyFile& file, std::string header) : file_(file), header_(std::move(header)) {}

void CsvLog::header_if_fresh(const DailyFile::Opened& opened) {
    if (opened.out != nullptr && opened.fresh) *opened.out << header_ << '\n';
}

std::string CsvLog::open_now(std::string_view stamp) {
    std::lock_guard lock(mutex_);
    const auto opened = file_.for_stamp(stamp);
    if (opened.out == nullptr) return file_.error();
    header_if_fresh(opened);
    opened.out->flush();
    return {};
}

void CsvLog::row(std::string_view stamp, const std::vector<std::string>& fields) {
    std::lock_guard lock(mutex_);
    const auto opened = file_.for_stamp(stamp);
    if (opened.out == nullptr) return;
    header_if_fresh(opened);
    *opened.out << csv_field(stamp);
    for (const auto& field : fields) *opened.out << ',' << csv_field(field);
    *opened.out << '\n';
    opened.out->flush();
}

}  // namespace cpm

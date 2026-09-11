#include "ssstudio/diff.h"

#include <algorithm>
#include <sstream>

namespace ssstudio {
namespace {

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

}  // namespace

std::vector<DiffLine> diff_lines(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = split_lines(before);
    const std::vector<std::string> b = split_lines(after);

    // lcs[i][j] = length of the longest common subsequence of a[i:] and b[j:].
    std::vector<std::vector<std::size_t>> lcs(a.size() + 1,
                                              std::vector<std::size_t>(b.size() + 1, 0));
    for (std::size_t i = a.size(); i-- > 0;) {
        for (std::size_t j = b.size(); j-- > 0;) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1
                                     : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }

    std::vector<DiffLine> out;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            out.push_back({DiffOp::Keep, a[i], i + 1, j + 1});
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            out.push_back({DiffOp::Remove, a[i], i + 1, 0});
            ++i;
        } else {
            out.push_back({DiffOp::Insert, b[j], 0, j + 1});
            ++j;
        }
    }
    for (; i < a.size(); ++i) out.push_back({DiffOp::Remove, a[i], i + 1, 0});
    for (; j < b.size(); ++j) out.push_back({DiffOp::Insert, b[j], 0, j + 1});
    return out;
}

DiffStats diff_stats(const std::vector<DiffLine>& lines) {
    DiffStats stats;
    for (const auto& line : lines) {
        if (line.op == DiffOp::Insert) ++stats.added;
        if (line.op == DiffOp::Remove) ++stats.removed;
    }
    return stats;
}

std::string format_unified(const std::vector<DiffLine>& lines, const std::string& before_label,
                           const std::string& after_label, int context) {
    if (diff_stats(lines).identical()) return {};

    // Mark every line within `context` of a change so unchanged runs collapse.
    std::vector<bool> keep(lines.size(), false);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].op == DiffOp::Keep) continue;
        const std::size_t low = i > static_cast<std::size_t>(context) ? i - context : 0;
        const std::size_t high = std::min(lines.size(), i + context + 1);
        for (std::size_t k = low; k < high; ++k) keep[k] = true;
    }

    std::ostringstream os;
    os << "--- " << before_label << "\n+++ " << after_label << "\n";
    bool in_gap = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!keep[i]) {
            if (!in_gap) {
                os << "@@\n";
                in_gap = true;
            }
            continue;
        }
        in_gap = false;
        switch (lines[i].op) {
            case DiffOp::Keep: os << " " << lines[i].text << "\n"; break;
            case DiffOp::Insert: os << "+" << lines[i].text << "\n"; break;
            case DiffOp::Remove: os << "-" << lines[i].text << "\n"; break;
        }
    }
    return os.str();
}

}  // namespace ssstudio

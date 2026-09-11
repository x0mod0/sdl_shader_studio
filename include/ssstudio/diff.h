// Line diff for version snapshots and A/B compare.
#ifndef SSSTUDIO_DIFF_H
#define SSSTUDIO_DIFF_H

#include <cstddef>
#include <string>
#include <vector>

namespace ssstudio {

enum class DiffOp { Keep, Insert, Remove };

struct DiffLine {
    DiffOp op = DiffOp::Keep;
    std::string text;
    std::size_t old_line = 0;  // 1-based, 0 when the line is an insertion
    std::size_t new_line = 0;  // 1-based, 0 when the line is a removal
};

struct DiffStats {
    std::size_t added = 0;
    std::size_t removed = 0;
    bool identical() const { return added == 0 && removed == 0; }
};

// Classic LCS diff. Shader files are small, so the quadratic table is fine and
// the output is the minimal edit script a person expects to read.
std::vector<DiffLine> diff_lines(const std::string& before, const std::string& after);
DiffStats diff_stats(const std::vector<DiffLine>& lines);

// Unified-diff text, for copying into a commit message or a bug report.
std::string format_unified(const std::vector<DiffLine>& lines, const std::string& before_label,
                           const std::string& after_label, int context = 3);

}  // namespace ssstudio

#endif  // SSSTUDIO_DIFF_H

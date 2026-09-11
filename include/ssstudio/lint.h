// Local checks that run on the editor buffer, without a compiler.
//
// Deliberately not a second opinion on anything the compiler already says: the
// app runs the real front end on every edit, so a lint that repeats it would
// only be a slower way to be told the same thing. What is here are the two
// things the compiler cannot say - a name that is right in the other dialect,
// and a resource declaration that compiles and then fails at bind time.
#ifndef SSSTUDIO_LINT_H
#define SSSTUDIO_LINT_H

#include <string_view>

#include "ssstudio/types.h"

namespace ssstudio {

/// Diagnostics for `source`, all of them warnings: nothing here is certain
/// enough to stop a build, and the compiler owns the errors. Line and column are
/// 1-based, matching what the compiler produces, so the editor can mark them the
/// same way.
Diagnostics lint(std::string_view source, Language language, Stage stage);

}  // namespace ssstudio

#endif  // SSSTUDIO_LINT_H

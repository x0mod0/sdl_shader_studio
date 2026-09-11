// Starter sources per stage and language, and cross-stage validation.
#ifndef SSSTUDIO_TEMPLATES_H
#define SSSTUDIO_TEMPLATES_H

#include <string>

#include "ssstudio/reflection.h"
#include "ssstudio/types.h"

namespace ssstudio {

// A correct-by-construction starting point. Each template documents the SDL
// register-space rule for its stage, because that is the rule newcomers hit
// first and the one the validator complains about loudest.
std::string default_source(Stage stage, Language language);

// Conventional filename suffix, e.g. ".frag.hlsl" or ".comp.glsl".
std::string default_extension(Stage stage, Language language);

/// The id fragment that marks a shader's stage: "_vert", "_frag" or "_comp".
std::string stage_suffix(Stage stage);

/// The id suffix that marks a shader's stage and its language, e.g.
/// "_frag_hlsl". It spells the same two facts default_extension() spells as a
/// file extension, so the id "test_frag_hlsl" goes with the file
/// test.frag.hlsl and the two can always be read off one another.
std::string id_suffix(Stage stage, Language language);

/// The shader id a name means for a given stage and language.
///
/// Ids have to be unique across a project - they become enumerators in the
/// generated header - but a name is not: "test" is a perfectly reasonable thing
/// to call every shader in a set, and test.vert.hlsl, test.frag.hlsl and
/// test.frag.glsl are three different files. So the stage and the language are
/// folded into the id, exactly as they are into the filename. A name that
/// already carries them keeps them rather than having them spelled twice, and an
/// empty name stays empty for the caller to reject.
std::string shader_id_for(const std::string& name, Stage stage, Language language);

/// The name a shader id carries - the inverse of shader_id_for(), and the file
/// basename that goes with it.
///
/// The suffix comes off because default_extension() is about to spell it again:
/// "test_frag_hlsl" pairs with test.frag.hlsl, and test_frag_hlsl.frag.hlsl
/// would match nothing in the tree. A bare stage suffix is accepted as well, so
/// that ids written before the language joined the convention - "sprite_vert",
/// as scaffold_project() still spells it - keep working. An id that is nothing
/// but a suffix keeps it, because stripping would leave no filename at all.
std::string shader_basename(const std::string& id, Stage stage, Language language);

/// A vertex shader that covers the screen with one triangle built from the
/// vertex index.
///
/// It binds no vertex buffer and declares no input layout, which is what the
/// preview draws, and it writes no varyings at all - a fullscreen fragment
/// shader takes its coordinate from the fragment position rather than from an
/// interpolated one, and a varying nobody reads is a warning nobody wants.
std::string fullscreen_vertex_source(Language language);

// Checks that what the vertex stage writes matches what the fragment stage
// reads. Matching is by location, not name, so a project can mix an HLSL vertex
// shader with a GLSL fragment shader.
Diagnostics validate_varyings(const Reflection& vertex, const Reflection& fragment,
                              const std::string& file);

}  // namespace ssstudio

#endif  // SSSTUDIO_TEMPLATES_H

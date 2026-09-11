// Reading a whole chain out of a scene description.
//
// The description is written by somebody else, so what matters most is what is
// done with the parts that cannot be brought across: an audio pass, a channel
// fed by a webcam, an image named by a path this has no way to resolve.
#include <string>

#include "ssstudio/shader_import.h"
#include "test.h"

using namespace ssstudio;

namespace {

const char* kChain = R"({ "Shader": {
  "info": { "name": "Trails", "username": "A. Person" },
  "renderpass": [
    { "name": "Image", "type": "image", "code": "void mainImage(out vec4 c, in vec2 p){ c = vec4(1); }",
      "inputs": [ { "id": 257, "ctype": "buffer", "channel": 0,
                    "sampler": { "filter": "linear", "wrap": "clamp", "vflip": "true" } } ],
      "outputs": [] },
    { "name": "Common", "type": "common", "code": "#define TAU 6.28318530718" },
    { "name": "Buf A", "type": "buffer", "code": "void mainImage(out vec4 c, in vec2 p){ c = vec4(0); }",
      "inputs": [ { "id": 257, "ctype": "buffer", "channel": 0, "sampler": { "wrap": "repeat" } },
                  { "id": 30, "ctype": "texture", "src": "https://example.invalid/n.png", "channel": 1 },
                  { "id": 99, "ctype": "keyboard", "channel": 2 } ],
      "outputs": [ { "id": 257, "channel": 0 } ] },
    { "name": "Sound", "type": "sound", "code": "vec2 mainSound(int s, float t){ return vec2(0); }",
      "inputs": [], "outputs": [] }
  ] } })";

ImportedChain read(const char* json, bool& ok) {
    ImportedChain chain;
    Diagnostics diags;
    ok = read_imported_chain(json, chain, diags);
    return chain;
}

}  // namespace

TEST(chain_import_orders_buffers_before_the_image) {
    bool ok = false;
    const ImportedChain chain = read(kChain, ok);
    CHECK(ok);
    CHECK_EQ(chain.passes.size(), static_cast<std::size_t>(2));
    CHECK_STREQ(chain.passes[0].name, "Buf A");
    CHECK(!chain.passes[0].is_image);
    CHECK_STREQ(chain.passes[1].name, "Image");
    CHECK(chain.passes[1].is_image);
}

TEST(chain_import_keeps_the_shared_code_apart_from_the_passes) {
    bool ok = false;
    const ImportedChain chain = read(kChain, ok);
    // Shared code is not a pass; it is prepended to the ones that are.
    CHECK(chain.common.find("TAU") != std::string::npos);
    for (const ImportedPass& pass : chain.passes) CHECK(pass.name != "Common");
}

TEST(chain_import_follows_output_ids_rather_than_names) {
    // An input names a pass by matching an id to that pass's declared output.
    // Matching on names instead would go wrong the moment two passes shared one.
    bool ok = false;
    const ImportedChain chain = read(kChain, ok);
    CHECK_STREQ(chain.passes[1].channel_passes.at(0), "Buf A");
    CHECK_STREQ(chain.passes[0].channel_passes.at(0), "Buf A");  // reads itself
}

TEST(chain_import_carries_sampler_settings_through) {
    bool ok = false;
    const ImportedChain chain = read(kChain, ok);
    const auto& image_sampler = chain.passes[1].channel_sampler.at(0);
    CHECK_STREQ(image_sampler.at("filter"), "linear");
    CHECK_STREQ(image_sampler.at("wrap"), "clamp");
    // Written as the string "true" by the exporters that produce these.
    CHECK_STREQ(image_sampler.at("vflip"), "true");
}

TEST(chain_import_names_what_it_could_not_bring_across) {
    bool ok = false;
    const ImportedChain chain = read(kChain, ok);
    CHECK(ok);  // a chain missing one channel is still worth importing

    bool mentions_keyboard = false;
    bool mentions_sound = false;
    for (const Diagnostic& note : chain.notes) {
        if (note.message.find("keyboard") != std::string::npos) mentions_keyboard = true;
        if (note.message.find("sound") != std::string::npos) mentions_sound = true;
    }
    CHECK(mentions_keyboard);
    CHECK(mentions_sound);

    // The channels that could be brought across still were.
    CHECK_EQ(chain.passes[0].channel_urls.size(), static_cast<std::size_t>(1));
    CHECK_EQ(chain.passes[0].channel_passes.size(), static_cast<std::size_t>(1));
    CHECK_EQ(chain.passes[0].channel_passes.count(2), static_cast<std::size_t>(0));
}

TEST(chain_import_flags_an_image_path_it_cannot_resolve) {
    bool ok = false;
    const ImportedChain chain = read(R"({"renderpass": [
        {"name": "Image", "type": "image", "code": "void mainImage(out vec4 c, in vec2 p){}",
         "inputs": [{"id": 1, "ctype": "texture", "src": "/media/a/noise.png", "channel": 0}],
         "outputs": []}]})", ok);
    CHECK(ok);
    bool flagged = false;
    for (const Diagnostic& note : chain.notes) {
        if (note.message.find("full address") != std::string::npos) flagged = true;
    }
    CHECK(flagged);
    // Kept anyway, so the name is not lost.
    CHECK_STREQ(chain.passes[0].channel_urls.at(0), "/media/a/noise.png");
}

TEST(chain_import_accepts_a_description_without_its_wrapper) {
    bool ok = false;
    read(R"({"renderpass": [{"name": "Image", "type": "image",
             "code": "void mainImage(out vec4 c, in vec2 p){}", "inputs": [], "outputs": []}]})",
         ok);
    CHECK(ok);
}

TEST(chain_import_refuses_what_it_cannot_use) {
    ImportedChain chain;
    Diagnostics diags;
    // Not JSON at all.
    CHECK(!read_imported_chain("not json", chain, diags));
    CHECK(has_errors(diags));

    // No passes.
    diags.clear();
    CHECK(!read_imported_chain(R"({"renderpass": []})", chain, diags));

    // Passes, but nothing that draws the image.
    diags.clear();
    CHECK(!read_imported_chain(R"({"renderpass": [{"name": "Buf A", "type": "buffer",
          "code": "void mainImage(out vec4 c, in vec2 p){}", "inputs": [], "outputs": []}]})",
                               chain, diags));
    CHECK(has_errors(diags));
}

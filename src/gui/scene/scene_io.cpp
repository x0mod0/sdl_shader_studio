// Scene persistence.
//
// Scenes live in <project>/scenes/*.toml and are loaded only by the app. Nothing
// here writes to project.toml, so a scene has no path into a build profile even
// by accident.
#include <algorithm>
#include <fstream>
#include <ostream>
#include <sstream>

#include <toml++/toml.hpp>

#include "scene/scene.h"

namespace ssstudio::gui {
namespace {

Diagnostic error(std::string message, std::string file = {}) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-SCENE";
    d.message = std::move(message);
    d.file = std::move(file);
    return d;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

template <std::size_t N>
void write_floats(std::ostream& os, const char* key, const float (&values)[N]) {
    os << key << " = [";
    for (std::size_t i = 0; i < N; ++i) {
        if (i) os << ", ";
        os << values[i];
    }
    os << "]\n";
}

template <std::size_t N>
void read_floats(const toml::table& table, const char* key, float (&out)[N]) {
    const auto* array = table[key].as_array();
    if (!array) return;
    std::size_t i = 0;
    for (const auto& node : *array) {
        if (i >= N) break;
        out[i++] = static_cast<float>(node.value_or(0.0));
    }
}

}  // namespace

bool load_scene(const std::filesystem::path& path, Scene& out, Diagnostics& out_diags) {
    toml::table table;
    try {
        table = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        Diagnostic d = error(std::string(e.description()), path.string());
        d.line = static_cast<int>(e.source().begin.line);
        out_diags.push_back(std::move(d));
        return false;
    }

    out = Scene{};
    out.path = path;
    out.name = table["scene"]["name"].value_or(path.stem().string());
    if (const auto* scene_table = table["scene"].as_table()) {
        read_floats(*scene_table, "clear_color", out.clear_color);
        read_floats(*scene_table, "gravity", out.gravity);
    }

    if (const auto* layers = table["layers"].as_array()) {
        for (const auto& node : *layers) {
            const auto* t = node.as_table();
            if (!t) continue;
            RenderLayer layer;
            layer.index = static_cast<int>((*t)["index"].value_or(0));
            layer.name = (*t)["name"].value_or(std::string());
            layer.camera = (*t)["camera"].value_or(std::string());
            layer.additive = (*t)["additive"].value_or(false);
            layer.enabled = (*t)["enabled"].value_or(true);
            layer.target = (*t)["target"].value_or(std::string());
            out.layers.push_back(std::move(layer));
        }
    }

    if (const auto* entities = table["entities"].as_array()) {
        for (const auto& node : *entities) {
            const auto* t = node.as_table();
            if (!t) continue;
            Entity e;
            e.id = (*t)["id"].value_or(std::string());
            e.name = (*t)["name"].value_or(e.id);
            e.parent = (*t)["parent"].value_or(std::string());
            e.enabled = (*t)["enabled"].value_or(true);
            if (const auto* tags = (*t)["tags"].as_array()) {
                for (const auto& tag : *tags) e.tags.push_back(tag.value_or(std::string()));
            }
            if (const auto* transform = (*t)["transform"].as_table()) {
                read_floats(*transform, "position", e.transform.position);
                read_floats(*transform, "scale", e.transform.scale);
                e.transform.rotation = static_cast<float>((*transform)["rotation"].value_or(0.0));
                e.transform.z = static_cast<float>((*transform)["z"].value_or(0.0));
            }

            if (const auto* s = (*t)["sprite"].as_table()) {
                Sprite sprite;
                sprite.texture = (*s)["texture"].value_or(std::string());
                sprite.material = (*s)["material"].value_or(std::string());
                sprite.layer = static_cast<int>((*s)["layer"].value_or(0));
                sprite.sort_key = static_cast<int>((*s)["sort_key"].value_or(0));
                sprite.flip_x = (*s)["flip_x"].value_or(false);
                sprite.flip_y = (*s)["flip_y"].value_or(false);
                read_floats(*s, "source_rect", sprite.source_rect);
                read_floats(*s, "tint", sprite.tint);
                read_floats(*s, "pivot", sprite.pivot);
                read_floats(*s, "size", sprite.size);
                e.sprite = std::move(sprite);
            }
            if (const auto* c = (*t)["camera2d"].as_table()) {
                Camera2D camera;
                camera.zoom = static_cast<float>((*c)["zoom"].value_or(1.0));
                camera.rotation = static_cast<float>((*c)["rotation"].value_or(0.0));
                camera.pixel_perfect = (*c)["pixel_perfect"].value_or(false);
                camera.active = (*c)["active"].value_or(true);
                read_floats(*c, "viewport", camera.viewport);
                e.camera2d = camera;
            }
            if (const auto* c = (*t)["camera3d"].as_table()) {
                Camera3D camera;
                camera.fov_degrees = static_cast<float>((*c)["fov"].value_or(60.0));
                camera.near_plane = static_cast<float>((*c)["near"].value_or(0.1));
                camera.far_plane = static_cast<float>((*c)["far"].value_or(100.0));
                camera.orthographic = (*c)["orthographic"].value_or(false);
                camera.orbit_distance = static_cast<float>((*c)["orbit_distance"].value_or(4.0));
                camera.orbit_yaw = static_cast<float>((*c)["orbit_yaw"].value_or(0.0));
                camera.orbit_pitch = static_cast<float>((*c)["orbit_pitch"].value_or(0.3));
                camera.active = (*c)["active"].value_or(true);
                e.camera3d = camera;
            }
            if (const auto* m = (*t)["mesh"].as_table()) {
                MeshRenderer mesh;
                mesh.mesh = (*m)["mesh"].value_or(std::string("cube"));
                mesh.material = (*m)["material"].value_or(std::string());
                mesh.wireframe = (*m)["wireframe"].value_or(false);
                e.mesh = std::move(mesh);
            }
            if (const auto* l = (*t)["light2d"].as_table()) {
                Light2D light;
                light.intensity = static_cast<float>((*l)["intensity"].value_or(1.0));
                light.radius = static_cast<float>((*l)["radius"].value_or(3.0));
                light.angle = static_cast<float>((*l)["angle"].value_or(0.0));
                light.cone_degrees = static_cast<float>((*l)["cone"].value_or(45.0));
                read_floats(*l, "color", light.color);
                e.light = light;
            }
            if (const auto* p = (*t)["post"].as_table()) {
                PostEffect post;
                post.material = (*p)["material"].value_or(std::string());
                post.order = static_cast<int>((*p)["order"].value_or(0));
                post.enabled = (*p)["enabled"].value_or(true);
                e.post = std::move(post);
            }
            if (const auto* tm = (*t)["tilemap"].as_table()) {
                Tilemap map;
                map.width = static_cast<int>((*tm)["width"].value_or(16));
                map.height = static_cast<int>((*tm)["height"].value_or(16));
                map.tileset_columns = static_cast<int>((*tm)["tileset_columns"].value_or(8));
                map.tileset = (*tm)["tileset"].value_or(std::string());
                map.material = (*tm)["material"].value_or(std::string());
                read_floats(*tm, "tile_size", map.tile_size);
                if (const auto* tiles = (*tm)["tiles"].as_array()) {
                    for (const auto& tile : *tiles) {
                        map.tiles.push_back(static_cast<int>(tile.value_or(0)));
                    }
                }
                e.tilemap = std::move(map);
            }
            if (const auto* tx = (*t)["text"].as_table()) {
                TextComponent text;
                text.text = (*tx)["text"].value_or(std::string("Hello"));
                text.size = static_cast<float>((*tx)["size"].value_or(1.0));
                text.font = (*tx)["font"].value_or(std::string());
                read_floats(*tx, "color", text.color);
                e.text = std::move(text);
            }
            out.entities.push_back(std::move(e));
        }
    }

    Diagnostics validation = out.validate();
    out_diags.insert(out_diags.end(), validation.begin(), validation.end());
    return !has_errors(validation);
}

bool save_scene(const Scene& scene, Diagnostics& out_diags) {
    if (scene.path.empty()) {
        out_diags.push_back(error("this scene has no path yet"));
        return false;
    }

    std::ostringstream os;
    os << "# " << scene.name << "\n"
       << "# A test scene. Scenes are never packed, exported, or referenced by generated\n"
       << "# code; the build produces identical output whether or not this file exists.\n\n";
    os << "[scene]\n";
    os << "name = \"" << escape(scene.name) << "\"\n";
    write_floats(os, "clear_color", scene.clear_color);
    write_floats(os, "gravity", scene.gravity);

    for (const auto& layer : scene.layers) {
        os << "\n[[layers]]\n";
        os << "index = " << layer.index << "\n";
        os << "name = \"" << escape(layer.name) << "\"\n";
        if (!layer.camera.empty()) os << "camera = \"" << escape(layer.camera) << "\"\n";
        if (layer.additive) os << "additive = true\n";
        if (!layer.enabled) os << "enabled = false\n";
        if (!layer.target.empty()) os << "target = \"" << escape(layer.target) << "\"\n";
    }

    for (const auto& e : scene.entities) {
        os << "\n[[entities]]\n";
        os << "id = \"" << escape(e.id) << "\"\n";
        os << "name = \"" << escape(e.name) << "\"\n";
        if (!e.parent.empty()) os << "parent = \"" << escape(e.parent) << "\"\n";
        if (!e.enabled) os << "enabled = false\n";
        if (!e.tags.empty()) {
            os << "tags = [";
            for (std::size_t i = 0; i < e.tags.size(); ++i) {
                if (i) os << ", ";
                os << "\"" << escape(e.tags[i]) << "\"";
            }
            os << "]\n";
        }

        os << "\n[entities.transform]\n";
        write_floats(os, "position", e.transform.position);
        write_floats(os, "scale", e.transform.scale);
        os << "rotation = " << e.transform.rotation << "\n";
        os << "z = " << e.transform.z << "\n";

        if (e.sprite) {
            os << "\n[entities.sprite]\n";
            if (!e.sprite->texture.empty()) {
                os << "texture = \"" << escape(e.sprite->texture.generic_string()) << "\"\n";
            }
            if (!e.sprite->material.empty()) {
                os << "material = \"" << escape(e.sprite->material) << "\"\n";
            }
            write_floats(os, "source_rect", e.sprite->source_rect);
            write_floats(os, "tint", e.sprite->tint);
            write_floats(os, "pivot", e.sprite->pivot);
            write_floats(os, "size", e.sprite->size);
            os << "layer = " << e.sprite->layer << "\n";
            os << "sort_key = " << e.sprite->sort_key << "\n";
            if (e.sprite->flip_x) os << "flip_x = true\n";
            if (e.sprite->flip_y) os << "flip_y = true\n";
        }
        if (e.camera2d) {
            os << "\n[entities.camera2d]\n";
            os << "zoom = " << e.camera2d->zoom << "\n";
            os << "rotation = " << e.camera2d->rotation << "\n";
            write_floats(os, "viewport", e.camera2d->viewport);
            os << "pixel_perfect = " << (e.camera2d->pixel_perfect ? "true" : "false") << "\n";
            os << "active = " << (e.camera2d->active ? "true" : "false") << "\n";
        }
        if (e.camera3d) {
            os << "\n[entities.camera3d]\n";
            os << "fov = " << e.camera3d->fov_degrees << "\n";
            os << "near = " << e.camera3d->near_plane << "\n";
            os << "far = " << e.camera3d->far_plane << "\n";
            os << "orthographic = " << (e.camera3d->orthographic ? "true" : "false") << "\n";
            os << "orbit_distance = " << e.camera3d->orbit_distance << "\n";
            os << "orbit_yaw = " << e.camera3d->orbit_yaw << "\n";
            os << "orbit_pitch = " << e.camera3d->orbit_pitch << "\n";
            os << "active = " << (e.camera3d->active ? "true" : "false") << "\n";
        }
        if (e.mesh) {
            os << "\n[entities.mesh]\n";
            os << "mesh = \"" << escape(e.mesh->mesh) << "\"\n";
            if (!e.mesh->material.empty()) os << "material = \"" << escape(e.mesh->material) << "\"\n";
            if (e.mesh->wireframe) os << "wireframe = true\n";
        }
        if (e.light) {
            os << "\n[entities.light2d]\n";
            write_floats(os, "color", e.light->color);
            os << "intensity = " << e.light->intensity << "\n";
            os << "radius = " << e.light->radius << "\n";
            os << "angle = " << e.light->angle << "\n";
            os << "cone = " << e.light->cone_degrees << "\n";
        }
        if (e.post) {
            os << "\n[entities.post]\n";
            os << "material = \"" << escape(e.post->material) << "\"\n";
            os << "order = " << e.post->order << "\n";
            os << "enabled = " << (e.post->enabled ? "true" : "false") << "\n";
        }
        if (e.tilemap) {
            os << "\n[entities.tilemap]\n";
            os << "width = " << e.tilemap->width << "\n";
            os << "height = " << e.tilemap->height << "\n";
            write_floats(os, "tile_size", e.tilemap->tile_size);
            os << "tileset_columns = " << e.tilemap->tileset_columns << "\n";
            if (!e.tilemap->tileset.empty()) {
                os << "tileset = \"" << escape(e.tilemap->tileset.generic_string()) << "\"\n";
            }
            if (!e.tilemap->material.empty()) {
                os << "material = \"" << escape(e.tilemap->material) << "\"\n";
            }
            os << "tiles = [";
            for (std::size_t i = 0; i < e.tilemap->tiles.size(); ++i) {
                if (i) os << ", ";
                os << e.tilemap->tiles[i];
            }
            os << "]\n";
        }
        if (e.text) {
            os << "\n[entities.text]\n";
            os << "text = \"" << escape(e.text->text) << "\"\n";
            os << "size = " << e.text->size << "\n";
            write_floats(os, "color", e.text->color);
            if (!e.text->font.empty()) {
                os << "font = \"" << escape(e.text->font.generic_string()) << "\"\n";
            }
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(scene.path.parent_path(), ec);
    const std::filesystem::path temp = scene.path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            out_diags.push_back(error("cannot write " + temp.string()));
            return false;
        }
        const std::string text = os.str();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::filesystem::rename(temp, scene.path, ec);
    if (ec) {
        out_diags.push_back(error("cannot replace " + scene.path.string() + ": " + ec.message()));
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> list_scenes(const std::filesystem::path& project_root) {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path dir = project_root / "scenes";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.path().extension() == ".toml") out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace ssstudio::gui

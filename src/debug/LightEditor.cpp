#include "LightEditor.hpp"

#include "../env/Lights.hpp"
#include "../env/WorldQuery.hpp"
#include "../features/DoodadLightTable.hpp"

#include "game/Doodad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace wxl_livingazeroth::debug
{
    namespace
    {
        namespace dd = wxl::game::doodad;
        namespace lt = lighttable;

        constexpr const char* kPanelTitle = "wxl-seyris-living-azeroth: light editor";
        const WXL_Api* g_api = nullptr;
        const WXL_SeyrisCdbcApi* g_cdbc = nullptr;
        float g_range = 25.0f;
        std::string g_selected;     // normalized model path
        char g_message[256] = "";

        // M2 header arrays (count, pointer once parsed): bones +0x2C, attachments +0xF0 (0x28 each:
        // id +0, bone +4), lights +0x108 (0x9C each: type +0, bone +2), emitters +0x128 (0x1DC each,
        // bone +0x14).
        template <class T> bool Get(const void* base, size_t off, T& out)
        {
            const void* p = static_cast<const uint8_t*>(base) + off;
            if (!base || !dd::detail::Readable(p, sizeof(T))) return false;
            std::memcpy(&out, p, sizeof(T));
            return true;
        }

        struct ModelInfo
        {
            std::string path;      // as the client has it
            unsigned    placed = 0;
            float       nearest = 1e9f;
            const uint8_t* header = nullptr;
            uint32_t    bones = 0, attachments = 0, emitters = 0, lights = 0;
        };

        // The models near the player, each model path once: everything in the client's M2 scene (map
        // and WMO doodads, game objects, creatures).
        std::vector<ModelInfo> NearbyModels(const world::Snapshot& s)
        {
            std::map<std::string, ModelInfo> byPath;
            static std::vector<lights::SceneModel> scene;
            lights::SceneModels(s.playerPos, g_range, scene);
            for (const lights::SceneModel& sm : scene)
            {
                if (!sm.path[0]) continue;
                ModelInfo& m = byPath[lt::Normalize(sm.path)];
                if (m.path.empty())
                {
                    m.path = sm.path;
                    m.header = sm.header;
                    if (m.header)
                    {
                        Get(m.header, 0x2C, m.bones); Get(m.header, 0xF0, m.attachments);
                        Get(m.header, 0x108, m.lights); Get(m.header, 0x128, m.emitters);
                    }
                }
                ++m.placed;
                m.nearest = std::min(m.nearest, sm.distance);
            }
            std::vector<ModelInfo> out;
            for (auto& [key, m] : byPath) out.push_back(m);
            std::sort(out.begin(), out.end(), [](const ModelInfo& a, const ModelInfo& b) { return a.nearest < b.nearest; });
            return out;
        }

        std::string Indexed(const uint8_t* header, size_t arrayAt, size_t stride, size_t boneAt, bool idFirst)
        {
            uint32_t count = 0;
            const uint8_t* items = nullptr;
            if (!header || !Get(header, arrayAt, count) || !Get(header, arrayAt + 4, items) || !items || !count) return "none";
            std::string s;
            char part[32];
            for (uint32_t k = 0; k < count && k < 64; ++k)
            {
                uint32_t id = k;
                uint16_t bone = 0;
                if (idFirst) Get(items + k * stride, 0, id);
                Get(items + k * stride, boneAt, bone);
                std::snprintf(part, sizeof(part), "%s%u:%u", s.empty() ? "" : "  ", id, bone);
                s += part;
            }
            return s;
        }

        const char* const kAttachNames[] = { "model origin", "attachment point (ID)", "bone (index)", "particle emitter (index)" };
        const char* const kTypeNames[] = { "point", "spot" };
        const char* const kFlickerNames[] = { "default (smooth, panel amount)", "off", "smooth (a flame)", "noise", "noise steps" };

        void EditAssignments(const ModelInfo& m)
        {
            // The light choices: "none" + every light by name.
            std::vector<std::string> names = { "none (suppress only)" };
            std::vector<uint32_t> ids = { 0 };
            for (const lt::Properties& p : lt::AllProperties())
            {
                names.push_back(std::to_string(p.id) + " " + p.name);
                ids.push_back(p.id);
            }
            std::vector<const char*> items;
            for (const std::string& n : names) items.push_back(n.c_str());

            std::vector<uint32_t> rows;
            for (const lt::Assignment* a : lt::ForModel(lt::Normalize(m.path))) rows.push_back(a->id);
            char label[160];
            for (uint32_t rowId : rows)
            {
                lt::Assignment* a = lt::EditAssignment(rowId);
                if (!a) continue;
                const lt::Properties* p = lt::FindProperties(a->lightId);
                // "###": the ID is only what follows, so the section stays open while its text changes.
                std::snprintf(label, sizeof(label), "assignment %u: %s at %s %d%s###as%u", a->id, p ? p->name.c_str() : "no light",
                              kAttachNames[a->attachType < lt::kAttachCount ? a->attachType : 0], a->attachIndex,
                              (a->flags & lt::kSuppressModelLights) ? ", model's own lights off" : "", a->id);
                if (!g_api->UiCollapsingHeader(label)) continue;
                bool changed = false;
                int type = static_cast<int>(a->attachType < lt::kAttachCount ? a->attachType : 0);
                std::snprintf(label, sizeof(label), "attach to##at%u", a->id);
                if (g_api->UiCombo(label, &type, kAttachNames, lt::kAttachCount)) { a->attachType = static_cast<uint32_t>(type); changed = true; }
                const int maxIndex = a->attachType == lt::kBone ? static_cast<int>(m.bones) - 1
                                   : a->attachType == lt::kParticleEmitter ? static_cast<int>(m.emitters) - 1 : 64;
                if (a->attachType != lt::kOrigin)
                {
                    int index = a->attachIndex;
                    std::snprintf(label, sizeof(label), "index / attachment ID##ai%u", a->id);
                    if (g_api->UiSliderInt(label, &index, 0, maxIndex > 0 ? maxIndex : 0)) { a->attachIndex = index; changed = true; }
                }
                const char* axes[] = { "X", "Y", "Z" };
                for (int c = 0; c < 3; ++c)
                {
                    std::snprintf(label, sizeof(label), "offset %s (yd)##o%d_%u", axes[c], c, a->id);
                    changed |= g_api->UiSliderFloat(label, &a->offset[c], -3.0f, 3.0f) != 0;
                }
                if (p && p->type == lt::kSpot)
                    for (int c = 0; c < 3; ++c)
                    {
                        std::snprintf(label, sizeof(label), "spot direction %s##d%d_%u", axes[c], c, a->id);
                        changed |= g_api->UiSliderFloat(label, &a->direction[c], -1.0f, 1.0f) != 0;
                    }
                int choice = 0;
                for (size_t k = 0; k < ids.size(); ++k) if (ids[k] == a->lightId) choice = static_cast<int>(k);
                std::snprintf(label, sizeof(label), "light##l%u", a->id);
                if (g_api->UiCombo(label, &choice, items.data(), static_cast<int>(items.size()))) { a->lightId = ids[choice]; changed = true; }
                int suppress = (a->flags & lt::kSuppressModelLights) ? 1 : 0;
                std::snprintf(label, sizeof(label), "suppress the model's own lights##s%u", a->id);
                if (g_api->UiCheckbox(label, &suppress)) { a->flags = suppress ? (a->flags | lt::kSuppressModelLights) : (a->flags & ~lt::kSuppressModelLights); changed = true; }
                if (changed) lt::Touch();
                std::snprintf(label, sizeof(label), "Delete assignment %u##del%u", a->id, a->id);
                if (g_api->UiButton(label)) { lt::RemoveAssignment(rowId); break; }
            }
            if (g_api->UiButton("Add an assignment to this model"))
            {
                const uint32_t id = lt::AddAssignment(m.path);
                if (lt::Assignment* a = lt::EditAssignment(id))
                {
                    // A useful start: a flame light at the first particle emitter, else the origin.
                    if (m.emitters) { a->attachType = lt::kParticleEmitter; a->attachIndex = 0; }
                    if (!lt::AllProperties().empty()) a->lightId = lt::AllProperties().front().id;
                    lt::Touch();
                }
            }
        }

        void EditLights()
        {
            std::vector<uint32_t> ids;
            for (const lt::Properties& p : lt::AllProperties()) ids.push_back(p.id);
            char label[160];
            for (uint32_t id : ids)
            {
                lt::Properties* p = lt::EditProperties(id);
                if (!p) continue;
                std::snprintf(label, sizeof(label), "light %u: %s###lp%u", p->id, p->name.c_str(), p->id);
                if (!g_api->UiCollapsingHeader(label)) continue;
                bool changed = false;
                char name[64];
                std::snprintf(name, sizeof(name), "%s", p->name.c_str());
                std::snprintf(label, sizeof(label), "name##n%u", p->id);
                if (g_api->UiInputText(label, name, sizeof(name))) { p->name = name; changed = true; }
                int type = p->type == lt::kSpot ? 1 : 0;
                std::snprintf(label, sizeof(label), "type##t%u", p->id);
                if (g_api->UiCombo(label, &type, kTypeNames, 2)) { p->type = static_cast<uint32_t>(type); changed = true; }
                float rgba[4] = { ((p->color >> 16) & 0xFF) / 255.0f, ((p->color >> 8) & 0xFF) / 255.0f, (p->color & 0xFF) / 255.0f, 1.0f };
                std::snprintf(label, sizeof(label), "colour##c%u", p->id);
                if (g_api->UiColorEdit(label, rgba))
                {
                    auto byte = [](float v) { return static_cast<uint32_t>((v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 255.0f + 0.5f); };
                    p->color = (byte(rgba[0]) << 16) | (byte(rgba[1]) << 8) | byte(rgba[2]);
                    changed = true;
                }
                std::snprintf(label, sizeof(label), "intensity##i%u", p->id);
                changed |= g_api->UiSliderFloat(label, &p->intensity, 0.0f, 5.0f) != 0;
                std::snprintf(label, sizeof(label), "radius (yd; -1 = default)##r%u", p->id);
                changed |= g_api->UiSliderFloat(label, &p->radius, -1.0f, 40.0f) != 0;
                std::snprintf(label, sizeof(label), "falloff (1 smooth, higher tighter; -1 = default)##f%u", p->id);
                changed |= g_api->UiSliderFloat(label, &p->falloff, -1.0f, 8.0f) != 0;
                if (p->type == lt::kSpot)
                {
                    std::snprintf(label, sizeof(label), "inner angle##ia%u", p->id);
                    changed |= g_api->UiSliderFloat(label, &p->innerAngle, 0.0f, 89.0f) != 0;
                    std::snprintf(label, sizeof(label), "outer angle##oa%u", p->id);
                    changed |= g_api->UiSliderFloat(label, &p->outerAngle, 1.0f, 90.0f) != 0;
                }
                int mode = p->flickerMode + 1; // -1..3 -> 0..4
                if (mode < 0 || mode > 4) mode = 0;
                std::snprintf(label, sizeof(label), "flicker##fm%u", p->id);
                if (g_api->UiCombo(label, &mode, kFlickerNames, 5)) { p->flickerMode = mode - 1; changed = true; }
                std::snprintf(label, sizeof(label), "flicker speed (-1 = default)##fs%u", p->id);
                changed |= g_api->UiSliderFloat(label, &p->flickerSpeed, -1.0f, 4.0f) != 0;
                std::snprintf(label, sizeof(label), "flicker amount (-1 = default)##fa%u", p->id);
                changed |= g_api->UiSliderFloat(label, &p->flickerAmount, -1.0f, 1.0f) != 0;
                if (changed) lt::Touch();
                std::snprintf(label, sizeof(label), "Delete light %u##dl%u", p->id, p->id);
                if (g_api->UiButton(label)) { lt::RemoveProperties(id); break; }
            }
            if (g_api->UiButton("Add a light")) lt::AddProperties();
        }

        void __cdecl Panel(void* /*user*/)
        {
            const world::Snapshot& s = world::Current();
            char line[512];
            std::snprintf(line, sizeof(line), "DoodadLightProperties / DoodadLightAssignment: %s%s", lt::Status(), lt::Dirty() ? " -- EDITED, not saved" : "");
            g_api->UiTextWrapped(line);
            if (g_api->UiButton("Save to DBFilesClient")) lt::Save(g_message, sizeof(g_message));
            g_api->UiSameLine();
            if (g_api->UiButton("Reload from disk (discards edits)")) { lt::Load(g_cdbc); std::snprintf(g_message, sizeof(g_message), "reloaded: %s", lt::Status()); }
            if (g_message[0]) g_api->UiTextWrapped(g_message);
            if (!lights::Config().enabled) g_api->UiTextWrapped("(our point lights are switched off in the lights panel: edits won't show)");
            if (!s.inWorld) { g_api->UiText("not in world"); return; }

            g_api->UiSeparator();
            g_api->UiSliderFloat("models within (yd)", &g_range, 3.0f, 60.0f);
            const std::vector<ModelInfo> models = NearbyModels(s);
            const ModelInfo* selected = nullptr;
            if (g_api->UiCollapsingHeader("Models near you"))
            {
                char label[64];
                int k = 0;
                for (const ModelInfo& m : models)
                {
                    const std::string key = lt::Normalize(m.path);
                    std::snprintf(label, sizeof(label), "%s##sel%d", key == g_selected ? "Selected" : "Select", k++);
                    if (g_api->UiButton(label)) g_selected = key;
                    g_api->UiSameLine();
                    std::snprintf(line, sizeof(line), "%.1f yd  %s  (%u placed; bones %u, attachments %u, emitters %u, own lights %u; %u assignment row(s))",
                                  m.nearest, m.path.c_str(), m.placed, m.bones, m.attachments, m.emitters, m.lights,
                                  static_cast<unsigned>(lt::ForModel(key).size()));
                    g_api->UiText(line);
                }
                if (models.empty()) g_api->UiText("(no placed models within range)");
            }
            for (const ModelInfo& m : models) if (lt::Normalize(m.path) == g_selected) selected = &m;

            g_api->UiSeparator();
            if (!selected) g_api->UiTextWrapped(g_selected.empty() ? "Select a model above to add lights to it." : "The selected model isn't within range any more.");
            else
            {
                std::snprintf(line, sizeof(line), "Selected: %s", selected->path.c_str()); g_api->UiTextWrapped(line);
                std::snprintf(line, sizeof(line), "  attachment points (ID:bone): %s", Indexed(selected->header, 0xF0, 0x28, 4, true).c_str()); g_api->UiTextWrapped(line);
                std::snprintf(line, sizeof(line), "  particle emitters (index:bone): %s", Indexed(selected->header, 0x128, 0x1DC, 0x14, false).c_str()); g_api->UiTextWrapped(line);
                std::snprintf(line, sizeof(line), "  own lights (index:bone): %s;  bones: %u", Indexed(selected->header, 0x108, 0x9C, 2, false).c_str(), selected->bones); g_api->UiTextWrapped(line);
                EditAssignments(*selected);
            }

            g_api->UiSeparator();
            g_api->UiText("Lights (DoodadLightProperties):");
            EditLights();
        }
    }

    void SetLightEditorCdbc(const WXL_SeyrisCdbcApi* cdbc) { g_cdbc = cdbc; }

    void RegisterLightEditor(const WXL_Api* api)
    {
        g_api = api;
        api->UiAddPanel(kPanelTitle, &Panel, nullptr);
    }
}

#include "LightEditor.hpp"

#include "../env/Lights.hpp"
#include "../env/WorldQuery.hpp"
#include "../features/DoodadLightTable.hpp"

#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/Doodad.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
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
            std::string attachedTo; // attached models (items in hands, ...): the parent's path, nearest instance
            bool        atRest = false; // ... and that instance wasn't animated (culled): placed at the parent's rest pose
        };

        // Path filters for the list (normalized paths: lowercase, backslashes).
        enum Category { kItem, kCreature, kCharacter, kWorld, kOther, kCategoryCount };
        const char* const kCategoryLabels[kCategoryCount] = { "item\\", "creature\\", "character\\", "world\\", "everything else" };
        int g_showCategory[kCategoryCount] = { 1, 1, 1, 1, 1 };

        Category CategoryOf(const std::string& normalized)
        {
            static const char* const prefixes[] = { "item\\", "creature\\", "character\\", "world\\" };
            for (int c = 0; c < kOther; ++c)
                if (normalized.compare(0, std::strlen(prefixes[c]), prefixes[c]) == 0) return static_cast<Category>(c);
            return kOther;
        }

        // The models near the player, each model path once: everything in the client's M2 scene (map
        // and WMO doodads, game objects, creatures) and the models attached to them (items in hands).
        std::vector<ModelInfo> NearbyModels(const world::Snapshot& s)
        {
            std::map<std::string, ModelInfo> byPath;
            static std::vector<lights::SceneModel> scene;
            lights::SceneModels(s.playerPos, g_range, scene);
            for (const lights::SceneModel& sm : scene) // nearest first
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
                    if (sm.parent)
                    {
                        m.attachedTo = lights::ModelPath(sm.parent);
                        if (m.attachedTo.empty()) m.attachedTo = "a model";
                        m.atRest = !sm.animated;
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

        // --- light markers in the world -------------------------------------------------------------
        // Each drawn light (lights::Active, after merging, at this frame's position): a cross at its
        // position in its colour (an indoor light also gets a small square), its reach as three rings
        // at its radius, and a spot light's axis. Drawn after the world without depth, so they show
        // through walls (the point is to see where a light really sits).
        namespace ev = wxl::events;
        namespace gx = wxl::game::gx;

        const char* const kMarkerModes[] = { "off", "the selected model's lights", "all lights within the range above" };
        int g_markerMode = 0;
        int g_markerRings = 1;

        struct LineVtx { float x, y, z; D3DCOLOR c; };

        D3DCOLOR MarkerColour(const float rgb[3], float scale)
        {
            const float top = std::max(std::max(rgb[0], rgb[1]), std::max(rgb[2], 1e-4f));
            auto byte = [&](float v) { return static_cast<DWORD>(std::min(255.0f, std::max(0.0f, v / top * scale * 255.0f))); };
            return D3DCOLOR_ARGB(255, byte(rgb[0]), byte(rgb[1]), byte(rgb[2]));
        }

        void AddMarker(std::vector<LineVtx>& v, const lights::ActiveLight& l)
        {
            const float* p = l.pos;
            const D3DCOLOR c = MarkerColour(l.color, 1.0f), dim = MarkerColour(l.color, 0.55f);
            constexpr float kCross = 0.4f, kSquare = 0.25f;
            for (int a = 0; a < 3; ++a)
            {
                float lo[3] = { p[0], p[1], p[2] }, hi[3] = { p[0], p[1], p[2] };
                lo[a] -= kCross; hi[a] += kCross;
                v.push_back({ lo[0], lo[1], lo[2], c }); v.push_back({ hi[0], hi[1], hi[2], c });
            }
            if (l.indoor)
            {
                const float s = kSquare;
                const float q[4][2] = { { -s, -s }, { s, -s }, { s, s }, { -s, s } };
                for (int k = 0; k < 4; ++k)
                {
                    const float* a = q[k]; const float* b = q[(k + 1) % 4];
                    v.push_back({ p[0] + a[0], p[1] + a[1], p[2], c }); v.push_back({ p[0] + b[0], p[1] + b[1], p[2], c });
                }
            }
            if (l.cosOuter > -1.5f)
            {
                const float len = l.radius * 0.5f;
                v.push_back({ p[0], p[1], p[2], c });
                v.push_back({ p[0] + l.spotDir[0] * len, p[1] + l.spotDir[1] * len, p[2] + l.spotDir[2] * len, c });
            }
            if (!g_markerRings || l.radius <= 0.0f) return;
            constexpr int kSegments = 32;
            const float r = l.radius;
            for (int plane = 0; plane < 3; ++plane) // around Z (horizontal), around X, around Y
                for (int k = 0; k < kSegments; ++k)
                {
                    const float a0 = 6.2831853f * k / kSegments, a1 = 6.2831853f * (k + 1) / kSegments;
                    float u0 = std::cos(a0) * r, w0 = std::sin(a0) * r, u1 = std::cos(a1) * r, w1 = std::sin(a1) * r;
                    float s0[3] = { p[0], p[1], p[2] }, s1[3] = { p[0], p[1], p[2] };
                    const int ia = plane == 0 ? 0 : (plane == 1 ? 1 : 0), ib = plane == 0 ? 1 : 2;
                    s0[ia] += u0; s0[ib] += w0; s1[ia] += u1; s1[ib] += w1;
                    v.push_back({ s0[0], s0[1], s0[2], dim }); v.push_back({ s1[0], s1[1], s1[2], dim });
                }
        }

        void __cdecl OnWorldSceneEnd(void* /*user*/, const void* args)
        {
            if (!g_markerMode) return;
            const world::Snapshot& s = world::Current();
            if (!s.inWorld) return;
            const auto* a = static_cast<const ev::WorldSceneEndArgs*>(args);
            auto* dev = static_cast<IDirect3DDevice9*>(a && a->device ? a->device : gx::RawDevice());
            if (!dev) return;

            std::vector<LineVtx> verts;
            std::map<const void*, bool> selectedModel; // per instance: does its path match the selection
            for (const lights::ActiveLight& l : lights::Active())
            {
                if (g_markerMode == 1)
                {
                    if (g_selected.empty() || !l.model) continue;
                    auto it = selectedModel.find(l.model);
                    if (it == selectedModel.end())
                        it = selectedModel.emplace(l.model, lt::Normalize(lights::ModelPath(l.model)) == g_selected).first;
                    if (!it->second) continue;
                }
                const float dx = l.pos[0] - s.playerPos[0], dy = l.pos[1] - s.playerPos[1], dz = l.pos[2] - s.playerPos[2];
                if (dx * dx + dy * dy + dz * dz > g_range * g_range && g_markerMode == 2) continue;
                AddMarker(verts, l);
            }
            if (verts.empty()) return;

            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(identity));
            dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(wxl::game::camera::GetView()));
            dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(wxl::game::camera::GetProjection()));
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetTexture(0, nullptr);
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
            dev->DrawPrimitiveUP(D3DPT_LINELIST, static_cast<UINT>(verts.size() / 2), verts.data(), sizeof(LineVtx));
            saved->Apply();
            saved->Release();
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
                int bake = (p->flags & lt::kBakeOcclusion) ? 1 : 0;
                std::snprintf(label, sizeof(label), "bake occlusion (walls and floors block it; only where it doesn't move)##bk%u", p->id);
                if (g_api->UiCheckbox(label, &bake)) { p->flags = bake ? (p->flags | lt::kBakeOcclusion) : (p->flags & ~lt::kBakeOcclusion); changed = true; }
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
            g_api->UiCombo("show lights in the world (through walls)", &g_markerMode, kMarkerModes, 3);
            g_api->UiCheckbox("... with their reach (rings at the radius; square = indoor, line = spot direction)", &g_markerRings);
            const std::vector<ModelInfo> models = NearbyModels(s);
            const ModelInfo* selected = nullptr;
            std::snprintf(line, sizeof(line), "%u different model(s) within %.0f yd", static_cast<unsigned>(models.size()), g_range);
            g_api->UiText(line);
            if (g_api->UiCollapsingHeader("Models near you"))
            {
                // Coloured text is a newer core call (UiTextColored); an older core gets tags instead.
                const bool colour = g_api->structSize >= offsetof(WXL_Api, UiTextColored) + sizeof(g_api->UiTextColored) && g_api->UiTextColored;
                static const float kOurs[4] = { 0.45f, 1.0f, 0.45f, 1.0f }, kStock[4] = { 1.0f, 0.8f, 0.35f, 1.0f };
                g_api->UiText("Show:");
                for (int c = 0; c < kCategoryCount; ++c)
                {
                    g_api->UiSameLine();
                    std::snprintf(line, sizeof(line), "%s##cat%d", kCategoryLabels[c], c);
                    g_api->UiCheckbox(line, &g_showCategory[c]);
                }
                if (colour)
                {
                    g_api->UiTextColored(kOurs, "green: has our light (DoodadLightAssignment)");
                    g_api->UiSameLine();
                    g_api->UiTextColored(kStock, "  amber: only its own (stock) lights");
                    g_api->UiSameLine();
                    g_api->UiText("  white: no light");
                }
                char label[64];
                int k = 0, shown = 0;
                for (const ModelInfo& m : models)
                {
                    const std::string key = lt::Normalize(m.path);
                    ++k;
                    if (!g_showCategory[CategoryOf(key)]) continue;
                    ++shown;
                    bool ours = false, suppressed = false;
                    const std::vector<const lt::Assignment*> rows = lt::ForModel(key);
                    for (const lt::Assignment* a : rows)
                    {
                        if (a->lightId) ours = true;
                        if (a->flags & lt::kSuppressModelLights) suppressed = true;
                    }
                    const bool stock = m.lights > 0 && !suppressed;
                    std::snprintf(label, sizeof(label), "%s##sel%d", key == g_selected ? "Selected" : "Select", k);
                    if (g_api->UiButton(label)) g_selected = key;
                    g_api->UiSameLine();
                    std::string where;
                    if (!m.attachedTo.empty()) where = std::string("  on ") + m.attachedTo + (m.atRest ? " (at rest: not animated now)" : "");
                    std::snprintf(line, sizeof(line), "%s%.1f yd  %s%s  (%u placed; bones %u, attachments %u, emitters %u, own lights %u%s; %u assignment row(s))",
                                  colour ? "" : (ours ? "[ours] " : (stock ? "[stock] " : "")),
                                  m.nearest, m.path.c_str(), where.c_str(), m.placed, m.bones, m.attachments, m.emitters, m.lights,
                                  suppressed && m.lights ? ", suppressed" : "", static_cast<unsigned>(rows.size()));
                    if (colour && (ours || stock)) g_api->UiTextColored(ours ? kOurs : kStock, line);
                    else g_api->UiText(line);
                }
                if (models.empty()) g_api->UiText("(no placed models within range)");
                else if (!shown) g_api->UiText("(none match the filters)");
            }
            for (const ModelInfo& m : models) if (lt::Normalize(m.path) == g_selected) selected = &m;
            // Out of range (teleported, walked away): drop the selection instead of staying stuck on it.
            if (!selected && !g_selected.empty())
            {
                std::snprintf(g_message, sizeof(g_message), "selection cleared: %s is no longer within %.0f yd", g_selected.c_str(), g_range);
                g_selected.clear();
            }

            g_api->UiSeparator();
            if (!selected) g_api->UiTextWrapped("Select a model above to add lights to it.");
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
        api->Subscribe(static_cast<uint32_t>(ev::Event::OnWorldSceneEnd), &OnWorldSceneEnd, nullptr);
    }
}

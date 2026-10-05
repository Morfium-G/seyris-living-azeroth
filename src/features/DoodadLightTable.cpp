#include "DoodadLightTable.hpp"

#include "../env/CdbcLoad.hpp"
#include "../wxl_seyris/CdbcApi.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace wxl_livingazeroth::lighttable
{
    namespace
    {
        constexpr const char* kPropertiesFile = "DBFilesClient\\DoodadLightProperties.cdbc";
        constexpr const char* kAssignmentFile = "DBFilesClient\\DoodadLightAssignment.cdbc";

        // Column layouts (append-only, docs/cdbc-tables.md). 'u' uint, 'i' int, 'f' float, 's' string.
        constexpr WXL_SeyrisCdbcField kPropertyFields[] = {
            {"ID", 0, WXL_CDBC_FIELD_VALUE},           {"Name", 1, WXL_CDBC_FIELD_STRING},
            {"Type", 2, WXL_CDBC_FIELD_VALUE},         {"Color", 3, WXL_CDBC_FIELD_VALUE},
            {"Intensity", 4, WXL_CDBC_FIELD_VALUE},    {"Radius", 5, WXL_CDBC_FIELD_VALUE},
            {"Falloff", 6, WXL_CDBC_FIELD_VALUE},      {"InnerAngle", 7, WXL_CDBC_FIELD_VALUE},
            {"OuterAngle", 8, WXL_CDBC_FIELD_VALUE},   {"FlickerMode", 9, WXL_CDBC_FIELD_VALUE},
            {"FlickerSpeed", 10, WXL_CDBC_FIELD_VALUE},{"FlickerAmount", 11, WXL_CDBC_FIELD_VALUE},
            {"Flags", 12, WXL_CDBC_FIELD_VALUE},
        };
        constexpr char kPropertyTypes[] = "usuufffffifff";
        constexpr WXL_SeyrisCdbcDefinition kPropertyDef = { "DoodadLightProperties", kPropertiesFile, kPropertyFields, 13 };

        constexpr WXL_SeyrisCdbcField kAssignmentFields[] = {
            {"ID", 0, WXL_CDBC_FIELD_VALUE},          {"ModelPath", 1, WXL_CDBC_FIELD_STRING},
            {"AttachType", 2, WXL_CDBC_FIELD_VALUE},  {"AttachIndex", 3, WXL_CDBC_FIELD_VALUE},
            {"AttachName", 4, WXL_CDBC_FIELD_STRING}, {"OffsetX", 5, WXL_CDBC_FIELD_VALUE},
            {"OffsetY", 6, WXL_CDBC_FIELD_VALUE},     {"OffsetZ", 7, WXL_CDBC_FIELD_VALUE},
            {"DirectionX", 8, WXL_CDBC_FIELD_VALUE},  {"DirectionY", 9, WXL_CDBC_FIELD_VALUE},
            {"DirectionZ", 10, WXL_CDBC_FIELD_VALUE}, {"LightID", 11, WXL_CDBC_FIELD_VALUE},
            {"Flags", 12, WXL_CDBC_FIELD_VALUE},
        };
        constexpr WXL_SeyrisCdbcDefinition kAssignmentDef = { "DoodadLightAssignment", kAssignmentFile, kAssignmentFields, 13 };

        std::vector<Properties> g_properties;
        std::vector<Assignment> g_assignments;
        std::map<std::string, std::vector<size_t>> g_byModel; // normalized path -> assignment indices
        std::string g_status = "not loaded yet";
        uint32_t    g_generation = 1;
        bool        g_dirty = false;

        void Reindex()
        {
            g_byModel.clear();
            for (size_t k = 0; k < g_assignments.size(); ++k) g_byModel[Normalize(g_assignments[k].modelPath)].push_back(k);
            ++g_generation;
        }

        float F(const WXL_SeyrisCdbcApi* cdbc, void* t, const void* row, const char* field, float missing)
        {
            return cdbcload::Float(cdbc, t, row, field, missing);
        }
        uint32_t U(const WXL_SeyrisCdbcApi* cdbc, void* t, const void* row, const char* field, uint32_t missing)
        {
            return cdbcload::Has(cdbc, t, field) ? cdbc->Value(t, row, field, 0) : missing;
        }

        // --- writing: plain WDBC, rows sorted by ID, a deduplicated string block --------------------
        struct Writer
        {
            std::vector<uint8_t> records;
            std::string strings = std::string(1, '\0');
            std::map<std::string, uint32_t> offsets{ { "", 0 } };
            void U(uint32_t v) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&v); records.insert(records.end(), b, b + 4); }
            void F(float v) { uint32_t u; std::memcpy(&u, &v, 4); U(u); }
            void S(const std::string& s)
            {
                auto it = offsets.find(s);
                if (it == offsets.end())
                {
                    it = offsets.emplace(s, static_cast<uint32_t>(strings.size())).first;
                    strings += s;
                    strings += '\0';
                }
                U(it->second);
            }
        };

        bool WriteFile(const char* path, const Writer& w, uint32_t rows, uint32_t columns, char* message, size_t messageSize)
        {
            std::vector<uint8_t> file = { 'W', 'D', 'B', 'C' };
            auto put = [&](uint32_t v) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&v); file.insert(file.end(), b, b + 4); };
            put(rows); put(columns); put(columns * 4); put(static_cast<uint32_t>(w.strings.size()));
            file.insert(file.end(), w.records.begin(), w.records.end());
            file.insert(file.end(), w.strings.begin(), w.strings.end());

            CreateDirectoryA("DBFilesClient", nullptr);
            const std::string backup = std::string(path) + ".bak";
            CopyFileA(path, backup.c_str(), FALSE); // no previous file: nothing to back up
            FILE* f = nullptr;
            if (fopen_s(&f, path, "wb") != 0 || !f) { std::snprintf(message, messageSize, "couldn't open %s for writing", path); return false; }
            const size_t written = std::fwrite(file.data(), 1, file.size(), f);
            std::fclose(f);
            if (written != file.size()) { std::snprintf(message, messageSize, "short write to %s", path); return false; }
            return true;
        }
    }

    std::string Normalize(const std::string& path)
    {
        std::string s = path;
        for (char& c : s)
        {
            if (c == '/') c = '\\';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    void Load(const WXL_SeyrisCdbcApi* cdbc)
    {
        g_properties.clear();
        g_assignments.clear();
        g_dirty = false;
        if (!cdbc || !cdbc->HasFeature("cdbc-load")) { g_status = "wxl-seyris-tools (cdbc) not available"; Reindex(); return; }
        std::string status;
        char err[256] = {};
        if (void* t = cdbcload::LoadAppendOnly(cdbc, kPropertyDef, err, sizeof(err)))
        {
            for (uint32_t i = 0, n = cdbc->RowCount(t); i < n; ++i)
            {
                const void* row = cdbc->RowAt(t, i);
                if (!row) continue;
                Properties p;
                p.id = cdbc->Value(t, row, "ID", 0);
                p.name = cdbcload::Has(cdbc, t, "Name") ? cdbc->GetString(t, row, "Name") : "";
                p.type = U(cdbc, t, row, "Type", kPoint);
                p.color = U(cdbc, t, row, "Color", p.color);
                p.intensity = F(cdbc, t, row, "Intensity", p.intensity);
                p.radius = F(cdbc, t, row, "Radius", -1.0f);
                p.falloff = F(cdbc, t, row, "Falloff", -1.0f);
                p.innerAngle = F(cdbc, t, row, "InnerAngle", p.innerAngle);
                p.outerAngle = F(cdbc, t, row, "OuterAngle", p.outerAngle);
                p.flickerMode = static_cast<int32_t>(U(cdbc, t, row, "FlickerMode", static_cast<uint32_t>(kFlickerDefault)));
                p.flickerSpeed = F(cdbc, t, row, "FlickerSpeed", -1.0f);
                p.flickerAmount = F(cdbc, t, row, "FlickerAmount", -1.0f);
                p.flags = U(cdbc, t, row, "Flags", 0);
                g_properties.push_back(p);
            }
            cdbc->Release(t);
        }
        else status += std::string(" (DoodadLightProperties: ") + err + ")";
        err[0] = 0;
        if (void* t = cdbcload::LoadAppendOnly(cdbc, kAssignmentDef, err, sizeof(err)))
        {
            for (uint32_t i = 0, n = cdbc->RowCount(t); i < n; ++i)
            {
                const void* row = cdbc->RowAt(t, i);
                if (!row) continue;
                Assignment a;
                a.id = cdbc->Value(t, row, "ID", 0);
                a.modelPath = cdbc->GetString(t, row, "ModelPath");
                a.attachType = U(cdbc, t, row, "AttachType", kOrigin);
                a.attachIndex = static_cast<int32_t>(U(cdbc, t, row, "AttachIndex", 0));
                a.attachName = cdbcload::Has(cdbc, t, "AttachName") ? cdbc->GetString(t, row, "AttachName") : "";
                a.offset[0] = F(cdbc, t, row, "OffsetX", 0.0f); a.offset[1] = F(cdbc, t, row, "OffsetY", 0.0f); a.offset[2] = F(cdbc, t, row, "OffsetZ", 0.0f);
                a.direction[0] = F(cdbc, t, row, "DirectionX", 0.0f); a.direction[1] = F(cdbc, t, row, "DirectionY", 0.0f); a.direction[2] = F(cdbc, t, row, "DirectionZ", -1.0f);
                a.lightId = U(cdbc, t, row, "LightID", 0);
                a.flags = U(cdbc, t, row, "Flags", 0);
                g_assignments.push_back(a);
            }
            cdbc->Release(t);
        }
        else status += std::string(" (DoodadLightAssignment: ") + err + ")";
        char line[160];
        std::snprintf(line, sizeof(line), "%u light(s), %u assignment(s)", static_cast<unsigned>(g_properties.size()), static_cast<unsigned>(g_assignments.size()));
        g_status = line + status;
        Reindex();
    }

    const char* Status() { return g_status.c_str(); }
    uint32_t    Generation() { return g_generation; }
    const std::vector<Properties>& AllProperties() { return g_properties; }
    const std::vector<Assignment>& AllAssignments() { return g_assignments; }

    const Properties* FindProperties(uint32_t id)
    {
        for (const Properties& p : g_properties) if (p.id == id) return &p;
        return nullptr;
    }

    std::vector<const Assignment*> ForModel(const std::string& normalizedPath)
    {
        std::vector<const Assignment*> out;
        auto it = g_byModel.find(normalizedPath);
        if (it != g_byModel.end()) for (size_t k : it->second) out.push_back(&g_assignments[k]);
        return out;
    }

    Properties* EditProperties(uint32_t id)
    {
        for (Properties& p : g_properties) if (p.id == id) return &p;
        return nullptr;
    }

    Assignment* EditAssignment(uint32_t id)
    {
        for (Assignment& a : g_assignments) if (a.id == id) return &a;
        return nullptr;
    }

    uint32_t AddProperties()
    {
        uint32_t id = 1;
        for (const Properties& p : g_properties) id = std::max(id, p.id + 1);
        Properties p;
        p.id = id;
        p.name = "Light " + std::to_string(id);
        g_properties.push_back(p);
        Touch();
        return id;
    }

    uint32_t AddAssignment(const std::string& modelPath)
    {
        uint32_t id = 1;
        for (const Assignment& a : g_assignments) id = std::max(id, a.id + 1);
        Assignment a;
        a.id = id;
        a.modelPath = modelPath;
        g_assignments.push_back(a);
        Touch();
        return id;
    }

    void RemoveProperties(uint32_t id)
    {
        g_properties.erase(std::remove_if(g_properties.begin(), g_properties.end(), [&](const Properties& p) { return p.id == id; }), g_properties.end());
        Touch();
    }

    void RemoveAssignment(uint32_t id)
    {
        g_assignments.erase(std::remove_if(g_assignments.begin(), g_assignments.end(), [&](const Assignment& a) { return a.id == id; }), g_assignments.end());
        Touch();
    }

    void Touch() { g_dirty = true; Reindex(); }
    bool Dirty() { return g_dirty; }

    bool Save(char* message, size_t messageSize)
    {
        std::vector<Properties> props = g_properties;
        std::sort(props.begin(), props.end(), [](const Properties& a, const Properties& b) { return a.id < b.id; });
        Writer pw;
        for (const Properties& p : props)
        {
            pw.U(p.id); pw.S(p.name); pw.U(p.type); pw.U(p.color); pw.F(p.intensity); pw.F(p.radius); pw.F(p.falloff);
            pw.F(p.innerAngle); pw.F(p.outerAngle); pw.U(static_cast<uint32_t>(p.flickerMode)); pw.F(p.flickerSpeed);
            pw.F(p.flickerAmount); pw.U(p.flags);
        }
        static_assert(sizeof(kPropertyTypes) - 1 == 13, "DoodadLightProperties columns");
        if (!WriteFile(kPropertiesFile, pw, static_cast<uint32_t>(props.size()), 13, message, messageSize)) return false;

        std::vector<Assignment> rows = g_assignments;
        std::sort(rows.begin(), rows.end(), [](const Assignment& a, const Assignment& b) { return a.id < b.id; });
        Writer aw;
        for (const Assignment& a : rows)
        {
            aw.U(a.id); aw.S(a.modelPath); aw.U(a.attachType); aw.U(static_cast<uint32_t>(a.attachIndex)); aw.S(a.attachName);
            aw.F(a.offset[0]); aw.F(a.offset[1]); aw.F(a.offset[2]);
            aw.F(a.direction[0]); aw.F(a.direction[1]); aw.F(a.direction[2]);
            aw.U(a.lightId); aw.U(a.flags);
        }
        if (!WriteFile(kAssignmentFile, aw, static_cast<uint32_t>(rows.size()), 13, message, messageSize)) return false;
        g_dirty = false;
        std::snprintf(message, messageSize, "saved %u light(s) and %u assignment(s) (old files kept as .bak)",
                      static_cast<unsigned>(props.size()), static_cast<unsigned>(rows.size()));
        return true;
    }
}

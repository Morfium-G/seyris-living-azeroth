// Loading this module's tables when a file is older than the code: columns are only ever appended
// (docs/cdbc-tables.md), so a file may lack the newest ones. LoadAppendOnly finds how many columns
// the file has and loads it with the fields that exist; Has() tells whether a field was there.
// Missing fields read as "not set" (the caller's inherit value), so appending a column never needs
// a converter.
#pragma once

#include "../wxl_seyris/CdbcApi.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wxl_livingazeroth::cdbcload
{
    /// Loads `def`; if the file has fewer columns than `def` names, loads the fields it has. NULL
    /// (with the reader's reason in err) when the file can't be read at all.
    inline void* LoadAppendOnly(const WXL_SeyrisCdbcApi* cdbc, const WXL_SeyrisCdbcDefinition& def, char* err, size_t errSize)
    {
        if (void* table = cdbc->Load(&def, err, errSize)) return table;

        // How many columns does the file have? A one-field definition loads only if its column exists.
        uint32_t maxIndex = 0;
        for (uint32_t i = 0; i < def.fieldCount; ++i) if (def.fields[i].index > maxIndex) maxIndex = def.fields[i].index;
        uint32_t columns = 0;
        char probeErr[64];
        for (uint32_t index = maxIndex; index > 0 && !columns; --index)
        {
            const WXL_SeyrisCdbcField probe[1] = { { "Probe", index, WXL_CDBC_FIELD_VALUE } };
            const WXL_SeyrisCdbcDefinition probeDef = { def.name, def.filename, probe, 1 };
            if (void* t = cdbc->Load(&probeDef, probeErr, sizeof(probeErr))) { cdbc->Release(t); columns = index + 1; }
        }
        if (!columns) return nullptr; // unreadable for another reason: err holds it

        std::vector<WXL_SeyrisCdbcField> present;
        for (uint32_t i = 0; i < def.fieldCount; ++i) if (def.fields[i].index < columns) present.push_back(def.fields[i]);
        const WXL_SeyrisCdbcDefinition trimmed = { def.name, def.filename, present.data(), static_cast<uint32_t>(present.size()) };
        return cdbc->Load(&trimmed, err, errSize);
    }

    /// Whether the loaded table has `field` (false for a column the file is too old to have).
    inline bool Has(const WXL_SeyrisCdbcApi* cdbc, void* table, const char* field)
    {
        return cdbc->FieldIndex(table, field) != static_cast<size_t>(-1);
    }

    inline float BitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, sizeof(f)); return f; }

    /// A float column, or `missing` when the file doesn't have it.
    inline float Float(const WXL_SeyrisCdbcApi* cdbc, void* table, const void* row, const char* field, float missing)
    {
        return Has(cdbc, table, field) ? BitsToFloat(cdbc->Value(table, row, field, 0)) : missing;
    }
}

// Published as "seyris.cdbc" via WXL_Api::PublishInterface/GetInterface (see PluginApi.h in the
// wxl-core fork). Not a core header -- wxl-seyris-tools is a separate module, so a consumer copies
// this file into its own repo rather than including it from core's include/wxl/. Mirrors the shape
// of core's own (not yet implemented) Db2Api.h, adapted for classic WDBC-format custom tables
// ("cdbc" = WDBC binary format, ".cdbc" extension used purely as a non-default-table naming
// convention -- see orchestration/docs/r&d/researched/dbc-format.md).
//
// Versioning (see WXL-12): the integer passed to PublishInterface/GetInterface ("seyris.cdbc", N)
// is a rarely-changing ABI-shape marker, bumped only on a breaking redesign -- not a release
// counter. Growth within one ABI shape is additive only: new fields are appended to the end of
// WXL_SeyrisCdbcApi, never inserted/reordered, so an older consumer's compiled-in layout stays a
// valid prefix of a newer, larger struct. Check structSize before reading a field an older build of
// this header may not carry. HasFeature() lets a consumer check for a specific newer capability by
// name and log a precise, non-technical-user-shareable diagnostic instead of crashing or reading a
// bare NULL.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SEYRIS_CDBC_INTERFACE_NAME    "seyris.cdbc"
#define WXL_SEYRIS_CDBC_INTERFACE_VERSION 1u

enum WXL_SeyrisCdbcFieldType
{
    WXL_CDBC_FIELD_VALUE  = 0, // raw uint32 (int/float bits/foreign key -- caller reinterprets)
    WXL_CDBC_FIELD_STRING = 1, // uint32 offset into the trailing string block
};

typedef struct WXL_SeyrisCdbcField
{
    const char* name;
    uint32_t    index; // column position within the record (0-based); column 0 must be the row id
    uint32_t    type;  // one of WXL_SeyrisCdbcFieldType
} WXL_SeyrisCdbcField;

// The record's actual column count and stride come from the file's own WDBC header at load time,
// not from fieldCount below -- a definition only needs to name the columns the caller cares about
// (by index), not enumerate every column a real table has. Rows are assumed ID-sorted on column 0,
// matching classic WDBC convention.
typedef struct WXL_SeyrisCdbcDefinition
{
    const char*                name;       // logical table name, used in error messages
    const char*                filename;   // e.g. "DBFilesClient\\ItemDisplayInfoExtra.cdbc"
    const WXL_SeyrisCdbcField* fields;      // the named columns this caller wants to read
    uint32_t                   fieldCount; // number of entries in `fields`, not the table's real column count
} WXL_SeyrisCdbcDefinition;

/**
 * @brief The service published as "seyris.cdbc".
 *
 * Check structSize before reading a field an older build of this header may not carry (see the
 * file-level comment on additive growth). Function pointers are appended here, in this exact
 * comment-documented order, as the reader gains capability -- see WXL-12 in the ticket system for
 * the full versioning rationale.
 */
typedef struct WXL_SeyrisCdbcApi
{
    uint32_t structSize;
    uint32_t major, minor, patch;

    /// Named capability check, independent of major/minor/patch. Always safe to call: present since
    /// the very first published version of this interface.
    int (__cdecl* HasFeature)(const char* name);

    // --- appended in WXL-12 milestone 2: load/query a classic WDBC-format table --------------------

    /**
     * @brief Reads a file and parses it against a schema.
     * @param def          field layout to read; only fields named here are queryable afterwards.
     * @param errorBuf     receives a human-readable reason on failure; may be NULL.
     * @param errorBufSize size of errorBuf.
     * @return an opaque table handle, or NULL on failure (file missing, bad magic, size mismatch).
     */
    void* (__cdecl* Load)(const WXL_SeyrisCdbcDefinition* def, char* errorBuf, size_t errorBufSize);

    /// Frees a table returned by Load. NULL is accepted and ignored.
    void (__cdecl* Release)(void* table);

    uint32_t (__cdecl* RowCount)(void* table);

    /// Row by position, [0, RowCount(table)). NULL if index is out of range.
    const void* (__cdecl* RowAt)(void* table, uint32_t index);

    /// Row by id (column 0), via binary search over the ID-sorted records. NULL if not found. If
    /// this table has been mutated since Load()/the last Save() (RowAdd only ever appends), the
    /// sort invariant may be temporarily broken -- Save() restores it, but between a RowAdd and the
    /// next Save(), prefer FindRowByField (order-independent) if you need to find a just-added row.
    const void* (__cdecl* FindRow)(void* table, uint32_t id);

    /// Raw uint32 value of a WXL_CDBC_FIELD_VALUE column. element must be 0 (no array-field support
    /// yet -- see the file-level comment on additive growth).
    uint32_t (__cdecl* Value)(void* table, const void* row, const char* field, uint32_t element);

    /// Resolved string of a WXL_CDBC_FIELD_STRING column. Never NULL for a valid field/row; empty
    /// string if the record's offset points at the string block's leading NUL.
    const char* (__cdecl* GetString)(void* table, const void* row, const char* field);

    /// Looks up a field's index in `def->fields` by name, for callers that don't want to keep the
    /// original definition around. SIZE_MAX if `field` isn't one of the schema's named columns.
    size_t (__cdecl* FieldIndex)(void* table, const char* field);

    // --- appended in WXL-12/WXL-10: foreign-key-style filtering, for tables with more than one row
    // per logical entity (e.g. ItemDisplayInfoExtra.cdbc's N models per display id) ---------------

    /**
     * @brief Finds the next row (in file order) whose `field` equals `value`.
     *
     * A resumable linear scan, not a prebuilt index -- fine for a modest custom table, and avoids
     * committing to index-maintenance machinery a caller may not need. Walk every match with:
     *   const void* row = nullptr;
     *   while ((row = FindRowByField(table, field, value, row)) != nullptr) { ... }
     * @param after  NULL to start from the beginning; a previously-returned row to continue past it.
     * @return the next matching row, or NULL once there are no more.
     */
    const void* (__cdecl* FindRowByField)(void* table, const char* field, uint32_t value, const void* after);

    // --- appended in WXL-17: write support (raw cdbc-file backend). HasFeature("cdbc-write") gates
    // this whole group. Any of these can reallocate/shift the table's internal storage -- a row
    // pointer returned before any of RowAdd/RowUpdateString/RowDelete is invalid after it; re-fetch
    // via FindRow/FindRowByField if you need it again. Save()/Reload() always invalidate every row
    // pointer for the table (Reload() invalidates the table handle itself too, returning a new one).
    // Only loose files are writable -- a cdbc file packed inside an MPQ can't be saved back to
    // (there's no write primitive for the client's own virtual filesystem, only for real loose
    // files), a deliberate scoping choice, not an oversight. Save() unconditionally backs up
    // whatever's currently on disk to "<filename>.bak" first, no setting gates this. ------------------

    /// Appends a new row and returns it: id is one past the current highest id (1 if the table is
    /// empty; gaps from deleted rows are left alone, not reused), every other column zero-filled
    /// (every string field reads as "" via offset 0).
    void* (__cdecl* RowAdd)(void* table);

    /// Sets a WXL_CDBC_FIELD_VALUE column on an already-added/found row. Non-zero on success.
    int (__cdecl* RowUpdateValue)(void* table, void* row, const char* field, uint32_t value);

    /// Sets a WXL_CDBC_FIELD_STRING column on an already-added/found row. Non-zero on success.
    int (__cdecl* RowUpdateString)(void* table, void* row, const char* field, const char* value);

    /// Removes a row. Non-zero on success; zero if `row` doesn't belong to this table.
    int (__cdecl* RowDelete)(void* table, void* row);

    /**
     * @brief Serializes the table's current in-memory state back to its original file.
     *
     * Requires every real column to be named in the schema this table was loaded with -- otherwise
     * an unnamed string-typed column's offset would be silently corrupted when the string block is
     * rebuilt. Fails (returns 0) rather than risk that. Rebuilds the string block from scratch each
     * call, reclaiming dead space left by RowUpdateString's append-only writes.
     * @param errorBuf     receives a human-readable reason on failure; may be NULL.
     * @param errorBufSize size of errorBuf.
     * @return non-zero on success.
     */
    int (__cdecl* Save)(void* table, char* errorBuf, size_t errorBufSize);

    /**
     * @brief Discards all in-memory state and re-reads the table fresh from disk.
     * @param errorBuf     receives a human-readable reason on failure; may be NULL.
     * @param errorBufSize size of errorBuf.
     * @return a new table handle, or NULL on failure (the old handle is untouched on failure, still
     *         usable; on success the old handle and every row pointer for it are invalid).
     */
    void* (__cdecl* Reload)(void* table, char* errorBuf, size_t errorBufSize);
} WXL_SeyrisCdbcApi;

#ifdef __cplusplus
}
#endif

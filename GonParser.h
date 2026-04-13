/*
 * GonParser.h -Minimal GON file parser for CSF config files
 *
 * Self-contained parser for a GON-like format. Uses standard C file I/O
 * to read config files from disk -no dependency on the game's GPAK or
 * GON subsystem. This keeps CSF independent of ExposeCatData.
 *
 * Supports:
 *   - Nested { } blocks (OBJECT type)
 *   - Key-value pairs with string, integer, or float values
 *   - Quoted strings ("hello world")
 *   - Unquoted identifiers (Bleed, Marked, etc.)
 *   - // line comments
 *   - Whitespace/newline insensitivity
 *
 * Limitations:
 *   - Fixed-size node pool (no malloc). Pool size set by GON_MAX_NODES.
 *   - String values stored inline (max GON_MAX_STRING_LEN chars).
 *   - Children stored as linked list (not random-access array).
 *   - No arrays (GON ARRAY type). Objects serve the same purpose.
 *
 * Usage:
 *   GonDoc doc;
 *   if (Gon_ParseFile(&doc, "data/mods/custom_statuses.gon")) {
 *       GonNode* root = Gon_Root(&doc);
 *       GonNode* statuses = Gon_Child(root, "custom_statuses");
 *       for (GonNode* s = Gon_FirstChild(statuses); s; s = Gon_Next(s)) {
 *           const char* donor = Gon_ChildStr(s, "donor", "Bleed");
 *           int category = Gon_ChildInt(s, "category", 1);
 *       }
 *   }
 *   // No cleanup needed -all storage is in the GonDoc struct.
 */

#ifndef GON_PARSER_H
#define GON_PARSER_H

#define GON_MAX_NODES       512
#define GON_MAX_STRING_LEN  64
#define GON_MAX_FILE_SIZE   (64 * 1024)   /* 64 KB max config file */

typedef enum {
    GON_TYPE_NULL   = 0,
    GON_TYPE_STRING = 1,
    GON_TYPE_NUMBER = 2,    /* stored as both int and double */
    GON_TYPE_OBJECT = 3,
} GonType;

typedef struct GonNode {
    char        name[GON_MAX_STRING_LEN];
    GonType     type;

    /* Value (valid when type != OBJECT) */
    char        str_val[GON_MAX_STRING_LEN];
    int         int_val;
    double      dbl_val;

    /* Tree links (valid when type == OBJECT or for the root) */
    int         first_child;    /* index into pool, -1 = none */
    int         next_sibling;   /* index into pool, -1 = none */
    int         child_count;
} GonNode;

typedef struct {
    GonNode     nodes[GON_MAX_NODES];
    int         node_count;
    char        file_buf[GON_MAX_FILE_SIZE];
    int         file_len;
    int         parse_ok;
} GonDoc;


/* -- Parsing ------------------------------------------------------ */

/* Parse a GON file from disk. Returns 1 on success, 0 on failure.
 * The GonDoc must be caller-allocated (stack or static). */
int Gon_ParseFile(GonDoc* doc, const char* filepath);

/* Parse a GON string in memory. Returns 1 on success. */
int Gon_ParseString(GonDoc* doc, const char* text, int textLen);


/* -- Navigation --------------------------------------------------- */

/* Get the root node (always node 0, type OBJECT). */
GonNode* Gon_Root(GonDoc* doc);

/* Find a named child of a parent node. Returns NULL if not found. */
GonNode* Gon_Child(GonDoc* doc, GonNode* parent, const char* name);

/* Get the first child of an OBJECT node. Returns NULL if no children. */
GonNode* Gon_FirstChild(GonDoc* doc, GonNode* parent);

/* Get the next sibling. Returns NULL if last. */
GonNode* Gon_Next(GonDoc* doc, GonNode* node);


/* -- Value accessors (with defaults) ------------------------------ */

/* Get string value from a named child. Returns def if not found. */
const char* Gon_ChildStr(GonDoc* doc, GonNode* parent,
                          const char* name, const char* def);

/* Get integer value from a named child. Returns def if not found. */
int Gon_ChildInt(GonDoc* doc, GonNode* parent,
                  const char* name, int def);

/* Get double value from a named child. Returns def if not found. */
double Gon_ChildDbl(GonDoc* doc, GonNode* parent,
                     const char* name, double def);


/* -- Merge / Patch operations ------------------------------------ */

/*
 * Port of Glaiel's GonObject merge semantics (see GON-master/gon.h).
 * Used to combine multiple custom_statuses.gon files from different mod
 * folders when more than one csf_core.dll instance is loaded.
 *
 * Mode resolution in PatchMerge comes from the *field name* suffix on
 * the source/patch node:
 *
 *   no suffix  -> MERGE  (recursive, same-name children merge,
 *                          new children append; default)
 *   .merge     -> MERGE  (explicit)
 *   .append    -> APPEND (push all children, duplicates allowed)
 *   .overwrite -> OVERWRITE (replace self with other)
 *   .add       -> STRING concat; NUMBER numeric add; OBJECT/ARRAY append
 *   .multiply  -> NUMBER numeric multiply; other types fall back to MERGE
 *
 * A child node whose name is literally ".append" / ".overwrite" / ".merge"
 * (empty base after stripping the suffix) means "patch self with this
 * child's contents" instead of inserting it as a new field.  See
 * Glaiel's README for details.
 *
 * We intentionally do not support the ARRAY field type here -our parser
 * never produces arrays; OBJECT serves the same purpose.
 */
typedef enum {
    GON_MERGE_DEFAULT   = 0,
    GON_MERGE_APPEND    = 1,
    GON_MERGE_MERGE     = 2,
    GON_MERGE_OVERWRITE = 3,
    GON_MERGE_ADD       = 4,
    GON_MERGE_MULTIPLY  = 5,
} GonMergeMode;

/* Deep-copy a subtree from srcDoc[srcIdx] into dstDoc, allocating new
 * nodes out of dstDoc's pool.  Returns the new node's index in dstDoc,
 * or -1 on pool exhaustion.  Node names and values are copied verbatim;
 * no suffix stripping happens here. */
int Gon_DeepCopyNode(GonDoc* dstDoc, const GonDoc* srcDoc, int srcIdx);

/* Patch-merge srcDoc's root into dstDoc's root, following Glaiel's
 * PatchMerge semantics (see gon.cpp line 878+).  Returns 1 on success.
 * After merging, any suffix-tagged names in inserted subtrees are
 * stripped so the resulting tree looks like a normal merged document.
 *
 * Both documents must already be parsed (parse_ok == 1).  dstDoc is
 * mutated in place; srcDoc is read-only. */
int Gon_PatchMergeDoc(GonDoc* dstDoc, const GonDoc* srcDoc);


#endif /* GON_PARSER_H */

/*
 * GonParser.c -Minimal GON file parser implementation
 *
 * Recursive descent parser for GON-like config files. All storage is
 * in the caller-provided GonDoc struct (no heap allocations).
 *
 * Grammar:
 *   document  := block*
 *   block     := IDENT '{' block* '}'
 *              | IDENT IDENT            (key-value: key value)
 *              | IDENT STRING           (key "quoted value")
 *              | IDENT NUMBER           (key 123 or key 1.5)
 *   IDENT     := [A-Za-z_][A-Za-z0-9_]*
 *   STRING    := '"' ... '"'
 *   NUMBER    := [-]?[0-9]+[.][0-9]*
 *   COMMENT   := '//' ... '\n'
 */

#include "GonParser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>


/* ====================================================================
 *  Tokenizer
 * ==================================================================== */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,      /* unquoted identifier */
    TOK_STRING,     /* "quoted string" */
    TOK_NUMBER,     /* integer or float */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_ERROR,
} TokType;

typedef struct {
    const char* src;
    int         pos;
    int         len;

    TokType     type;
    char        text[GON_MAX_STRING_LEN];
    int         int_val;
    double      dbl_val;
} Tokenizer;

static void Tok_Init(Tokenizer* t, const char* src, int len)
{
    t->src = src;
    t->pos = 0;
    t->len = len;
    t->type = TOK_EOF;
    t->text[0] = '\0';
    t->int_val = 0;
    t->dbl_val = 0.0;
}

static void Tok_SkipWhitespace(Tokenizer* t)
{
    while (t->pos < t->len)
    {
        char c = t->src[t->pos];

        /* Whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            t->pos++;
            continue;
        }

        /* Line comment: // */
        if (c == '/' && t->pos + 1 < t->len && t->src[t->pos + 1] == '/')
        {
            t->pos += 2;
            while (t->pos < t->len && t->src[t->pos] != '\n')
                t->pos++;
            continue;
        }

        break;
    }
}

static TokType Tok_Next(Tokenizer* t)
{
    int start, i;
    char c;

    Tok_SkipWhitespace(t);

    if (t->pos >= t->len)
    {
        t->type = TOK_EOF;
        t->text[0] = '\0';
        return TOK_EOF;
    }

    c = t->src[t->pos];

    /* Braces */
    if (c == '{') { t->pos++; t->type = TOK_LBRACE; t->text[0] = '{'; t->text[1] = '\0'; return TOK_LBRACE; }
    if (c == '}') { t->pos++; t->type = TOK_RBRACE; t->text[0] = '}'; t->text[1] = '\0'; return TOK_RBRACE; }

    /* Quoted string */
    if (c == '"')
    {
        t->pos++;  /* skip opening quote */
        start = t->pos;
        while (t->pos < t->len && t->src[t->pos] != '"')
            t->pos++;

        i = t->pos - start;
        if (i >= GON_MAX_STRING_LEN) i = GON_MAX_STRING_LEN - 1;
        memcpy(t->text, t->src + start, i);
        t->text[i] = '\0';

        if (t->pos < t->len) t->pos++;  /* skip closing quote */
        t->type = TOK_STRING;
        return TOK_STRING;
    }

    /* Number: starts with digit or minus followed by digit */
    if (isdigit((unsigned char)c) ||
        (c == '-' && t->pos + 1 < t->len && isdigit((unsigned char)t->src[t->pos + 1])))
    {
        int isFloat = 0;
        start = t->pos;
        if (c == '-') t->pos++;
        while (t->pos < t->len && isdigit((unsigned char)t->src[t->pos]))
            t->pos++;
        if (t->pos < t->len && t->src[t->pos] == '.')
        {
            isFloat = 1;
            t->pos++;
            while (t->pos < t->len && isdigit((unsigned char)t->src[t->pos]))
                t->pos++;
        }

        i = t->pos - start;
        if (i >= GON_MAX_STRING_LEN) i = GON_MAX_STRING_LEN - 1;
        memcpy(t->text, t->src + start, i);
        t->text[i] = '\0';

        t->dbl_val = atof(t->text);
        t->int_val = atoi(t->text);
        t->type = TOK_NUMBER;
        return TOK_NUMBER;
    }

    /* Identifier: letter or underscore start */
    if (isalpha((unsigned char)c) || c == '_')
    {
        start = t->pos;
        while (t->pos < t->len &&
               (isalnum((unsigned char)t->src[t->pos]) || t->src[t->pos] == '_'))
            t->pos++;

        i = t->pos - start;
        if (i >= GON_MAX_STRING_LEN) i = GON_MAX_STRING_LEN - 1;
        memcpy(t->text, t->src + start, i);
        t->text[i] = '\0';

        t->type = TOK_IDENT;
        return TOK_IDENT;
    }

    /* Unknown character -skip and report error */
    t->pos++;
    t->type = TOK_ERROR;
    return TOK_ERROR;
}

static TokType Tok_Peek(Tokenizer* t)
{
    int saved_pos = t->pos;
    TokType saved_type = t->type;
    char saved_text[GON_MAX_STRING_LEN];
    int saved_int = t->int_val;
    double saved_dbl = t->dbl_val;
    TokType result;

    memcpy(saved_text, t->text, GON_MAX_STRING_LEN);

    result = Tok_Next(t);

    /* Restore state */
    t->pos = saved_pos;
    t->type = saved_type;
    t->int_val = saved_int;
    t->dbl_val = saved_dbl;
    memcpy(t->text, saved_text, GON_MAX_STRING_LEN);

    return result;
}


/* ====================================================================
 *  Parser -recursive descent
 * ==================================================================== */

static int AllocNode(GonDoc* doc)
{
    if (doc->node_count >= GON_MAX_NODES)
        return -1;
    {
        int idx = doc->node_count++;
        memset(&doc->nodes[idx], 0, sizeof(GonNode));
        doc->nodes[idx].first_child  = -1;
        doc->nodes[idx].next_sibling = -1;
        return idx;
    }
}

static void AddChild(GonDoc* doc, int parentIdx, int childIdx)
{
    GonNode* parent = &doc->nodes[parentIdx];
    parent->child_count++;

    if (parent->first_child == -1)
    {
        parent->first_child = childIdx;
        return;
    }

    /* Walk to last child */
    {
        int cur = parent->first_child;
        while (doc->nodes[cur].next_sibling != -1)
            cur = doc->nodes[cur].next_sibling;
        doc->nodes[cur].next_sibling = childIdx;
    }
}

/* Parse the contents of a block (between braces or at top level).
 * Adds children to parentIdx. Returns 1 on success. */
static int ParseBlock(GonDoc* doc, Tokenizer* tok, int parentIdx)
{
    while (1)
    {
        TokType tt = Tok_Next(tok);
        int nodeIdx;
        TokType peekType;

        /* End of block or file */
        if (tt == TOK_EOF || tt == TOK_RBRACE)
            return 1;

        /* Expect an identifier (the key/name) */
        if (tt != TOK_IDENT)
            return 0;  /* parse error */

        nodeIdx = AllocNode(doc);
        if (nodeIdx < 0)
            return 0;  /* pool exhausted */

        strncpy(doc->nodes[nodeIdx].name, tok->text, GON_MAX_STRING_LEN - 1);

        /* Peek at next token to determine type */
        peekType = Tok_Peek(tok);

        if (peekType == TOK_LBRACE)
        {
            /* Object block: Name { ... } */
            Tok_Next(tok);  /* consume '{' */
            doc->nodes[nodeIdx].type = GON_TYPE_OBJECT;
            AddChild(doc, parentIdx, nodeIdx);

            if (!ParseBlock(doc, tok, nodeIdx))
                return 0;
        }
        else if (peekType == TOK_STRING)
        {
            /* String value: Name "value" */
            Tok_Next(tok);  /* consume string */
            doc->nodes[nodeIdx].type = GON_TYPE_STRING;
            strncpy(doc->nodes[nodeIdx].str_val, tok->text, GON_MAX_STRING_LEN - 1);
            AddChild(doc, parentIdx, nodeIdx);
        }
        else if (peekType == TOK_NUMBER)
        {
            /* Number value: Name 123 or Name 1.5 */
            Tok_Next(tok);  /* consume number */
            doc->nodes[nodeIdx].type = GON_TYPE_NUMBER;
            doc->nodes[nodeIdx].int_val = tok->int_val;
            doc->nodes[nodeIdx].dbl_val = tok->dbl_val;
            strncpy(doc->nodes[nodeIdx].str_val, tok->text, GON_MAX_STRING_LEN - 1);
            AddChild(doc, parentIdx, nodeIdx);
        }
        else if (peekType == TOK_IDENT)
        {
            /* Unquoted value: Name Bleed (treated as string) */
            Tok_Next(tok);  /* consume ident */
            doc->nodes[nodeIdx].type = GON_TYPE_STRING;
            strncpy(doc->nodes[nodeIdx].str_val, tok->text, GON_MAX_STRING_LEN - 1);
            AddChild(doc, parentIdx, nodeIdx);
        }
        else
        {
            /* Standalone identifier with no value -treat as null */
            doc->nodes[nodeIdx].type = GON_TYPE_NULL;
            AddChild(doc, parentIdx, nodeIdx);
        }
    }
}


/* ====================================================================
 *  Public API
 * ==================================================================== */

int Gon_ParseString(GonDoc* doc, const char* text, int textLen)
{
    Tokenizer tok;
    int rootIdx;

    /* Reset node pool and parse state, but preserve file_buf/file_len
     * so callers (Gon_ParseFile) can pass doc->file_buf as 'text'. */
    memset(doc->nodes, 0, sizeof(doc->nodes));
    doc->node_count = 0;
    doc->parse_ok   = 0;

    /* Allocate root node */
    rootIdx = AllocNode(doc);
    if (rootIdx != 0) return 0;  /* should always be 0 */
    doc->nodes[0].type = GON_TYPE_OBJECT;
    strcpy(doc->nodes[0].name, "__root__");

    /* Parse */
    Tok_Init(&tok, text, textLen);
    if (!ParseBlock(doc, &tok, 0))
        return 0;

    doc->parse_ok = 1;
    return 1;
}

int Gon_ParseFile(GonDoc* doc, const char* filepath)
{
    FILE* f;
    int bytesRead;

    memset(doc, 0, sizeof(GonDoc));
    doc->parse_ok = 0;

    f = fopen(filepath, "rb");
    if (!f) return 0;

    bytesRead = (int)fread(doc->file_buf, 1, GON_MAX_FILE_SIZE - 1, f);
    fclose(f);

    if (bytesRead <= 0) return 0;
    doc->file_buf[bytesRead] = '\0';
    doc->file_len = bytesRead;

    return Gon_ParseString(doc, doc->file_buf, bytesRead);
}


/* -- Navigation --------------------------------------------------- */

GonNode* Gon_Root(GonDoc* doc)
{
    if (!doc || !doc->parse_ok || doc->node_count == 0) return NULL;
    return &doc->nodes[0];
}

GonNode* Gon_Child(GonDoc* doc, GonNode* parent, const char* name)
{
    int idx;
    if (!doc || !parent || parent->first_child == -1) return NULL;

    idx = parent->first_child;
    while (idx != -1)
    {
        if (strcmp(doc->nodes[idx].name, name) == 0)
            return &doc->nodes[idx];
        idx = doc->nodes[idx].next_sibling;
    }
    return NULL;
}

GonNode* Gon_FirstChild(GonDoc* doc, GonNode* parent)
{
    if (!doc || !parent || parent->first_child == -1) return NULL;
    return &doc->nodes[parent->first_child];
}

GonNode* Gon_Next(GonDoc* doc, GonNode* node)
{
    if (!doc || !node || node->next_sibling == -1) return NULL;
    return &doc->nodes[node->next_sibling];
}


/* -- Value accessors ---------------------------------------------- */

const char* Gon_ChildStr(GonDoc* doc, GonNode* parent,
                          const char* name, const char* def)
{
    GonNode* child = Gon_Child(doc, parent, name);
    if (!child) return def;
    if (child->type == GON_TYPE_STRING || child->type == GON_TYPE_NUMBER)
        return child->str_val;
    return def;
}

int Gon_ChildInt(GonDoc* doc, GonNode* parent,
                  const char* name, int def)
{
    GonNode* child = Gon_Child(doc, parent, name);
    if (!child || child->type != GON_TYPE_NUMBER) return def;
    return child->int_val;
}

double Gon_ChildDbl(GonDoc* doc, GonNode* parent,
                     const char* name, double def)
{
    GonNode* child = Gon_Child(doc, parent, name);
    if (!child || child->type != GON_TYPE_NUMBER) return def;
    return child->dbl_val;
}


/* ====================================================================
 *  Merge / Patch operations
 *
 *  C port of Glaiel's GonObject::PatchMerge semantics.  See gon.cpp
 *  around line 878 for the C++ reference implementation.  Adapted to
 *  our pool-based, linked-list child storage and simplified by the
 *  absence of an ARRAY type.
 * ==================================================================== */

/* -- Suffix helpers -- */

static int Gon__EndsWith(const char* s, const char* suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (sl < xl) return 0;
    return strcmp(s + (sl - xl), suffix) == 0;
}

static GonMergeMode Gon__GetPatchMode(const char* name)
{
    if (Gon__EndsWith(name, ".overwrite")) return GON_MERGE_OVERWRITE;
    if (Gon__EndsWith(name, ".append"))    return GON_MERGE_APPEND;
    if (Gon__EndsWith(name, ".merge"))     return GON_MERGE_MERGE;
    if (Gon__EndsWith(name, ".add"))       return GON_MERGE_ADD;
    if (Gon__EndsWith(name, ".multiply"))  return GON_MERGE_MULTIPLY;
    return GON_MERGE_DEFAULT;
}

static int Gon__HasPatchSuffix(const char* name)
{
    return Gon__GetPatchMode(name) != GON_MERGE_DEFAULT;
}

/* In-place strip of a single trailing patch suffix on name. */
static void Gon__StripPatchSuffix(char* name)
{
    static const char* const kSuffixes[] = {
        ".overwrite", ".append", ".merge", ".add", ".multiply", NULL
    };
    size_t sl = strlen(name);
    int i;
    for (i = 0; kSuffixes[i]; i++)
    {
        size_t xl = strlen(kSuffixes[i]);
        if (sl >= xl && strcmp(name + (sl - xl), kSuffixes[i]) == 0)
        {
            name[sl - xl] = '\0';
            return;
        }
    }
}

/* Recursively strip patch suffixes from a subtree's node names. */
static void Gon__StripSuffixesRecursive(GonDoc* doc, int idx)
{
    int c;
    if (idx < 0 || idx >= doc->node_count) return;
    Gon__StripPatchSuffix(doc->nodes[idx].name);
    c = doc->nodes[idx].first_child;
    while (c != -1)
    {
        Gon__StripSuffixesRecursive(doc, c);
        c = doc->nodes[c].next_sibling;
    }
}

/* Look up the Nth child matching 'name' (0 = first match).  Returns
 * the pool index or -1 if not found.  Used by the merge logic to
 * handle duplicate-name children correctly. */
static int Gon__NthChildWithNameIdx(GonDoc* doc, int parentIdx,
                                     const char* name, int nth)
{
    int c;
    int count = 0;
    if (parentIdx < 0 || parentIdx >= doc->node_count) return -1;
    c = doc->nodes[parentIdx].first_child;
    while (c != -1)
    {
        if (strcmp(doc->nodes[c].name, name) == 0)
        {
            if (count == nth) return c;
            count++;
        }
        c = doc->nodes[c].next_sibling;
    }
    return -1;
}


/* -- Deep copy (internal node allocator bridge) -- */

/* Forward-decls to the static helpers that already exist earlier in
 * this file.  Defined as static so we can't call them from the header,
 * but they live in the same TU so they're in scope here. */
/* static int AllocNode(GonDoc* doc);                                */
/* static void AddChild(GonDoc* doc, int parentIdx, int childIdx);    */

int Gon_DeepCopyNode(GonDoc* dst, const GonDoc* src, int srcIdx)
{
    int newIdx;
    const GonNode* s;
    GonNode* n;
    int srcChild;

    if (!dst || !src) return -1;
    if (srcIdx < 0 || srcIdx >= src->node_count) return -1;

    newIdx = AllocNode(dst);
    if (newIdx < 0) return -1;

    s = &src->nodes[srcIdx];
    n = &dst->nodes[newIdx];

    memcpy(n->name, s->name, GON_MAX_STRING_LEN);
    n->type = s->type;
    memcpy(n->str_val, s->str_val, GON_MAX_STRING_LEN);
    n->int_val     = s->int_val;
    n->dbl_val     = s->dbl_val;
    n->first_child = -1;
    n->next_sibling = -1;
    n->child_count = 0;

    /* Recursively copy children, preserving order. */
    srcChild = s->first_child;
    while (srcChild != -1)
    {
        int childNew = Gon_DeepCopyNode(dst, src, srcChild);
        if (childNew < 0) return -1;          /* pool exhausted */
        AddChild(dst, newIdx, childNew);
        srcChild = src->nodes[srcChild].next_sibling;
    }
    return newIdx;
}


/* -- Core PatchMerge -- */

/* Patch-merge srcDoc[srcIdx] into dstDoc[dstIdx].  Mirrors the C++
 * GonObject::PatchMerge logic, with these simplifications:
 *   - No ARRAY branch (our parser has no arrays).
 *   - Duplicate-name tracking uses a fixed 64-slot table per frame,
 *     which is enough for any realistic custom_statuses.gon block.
 *
 * Returns 1 on success, 0 on pool exhaustion or duplicate-name overflow.
 */
static int Gon__PatchMergeIdx(GonDoc* dst, int dstIdx,
                              const GonDoc* src, int srcIdx)
{
    GonNode* self;
    const GonNode* other;
    GonMergeMode policy;

    if (!dst || !src) return 0;
    if (dstIdx < 0 || dstIdx >= dst->node_count) return 0;
    if (srcIdx < 0 || srcIdx >= src->node_count) return 0;

    self  = &dst->nodes[dstIdx];
    other = &src->nodes[srcIdx];

    /* Policy comes from the *patch* node's own name.  DEFAULT means
     * "MERGE" semantics for OBJECT, which is what we want at root. */
    policy = Gon__GetPatchMode(other->name);

    /* -- OBJECT + OBJECT -- */
    if (self->type == GON_TYPE_OBJECT && other->type == GON_TYPE_OBJECT)
    {
        int oc;

        if (policy == GON_MERGE_OVERWRITE)
        {
            /* Drop our children (orphan them in the pool -acceptable:
             * the pool is growing-only anyway) and deep-copy other's
             * children in, stripping suffixes as we go. */
            self->first_child = -1;
            self->child_count = 0;

            oc = other->first_child;
            while (oc != -1)
            {
                int newChild = Gon_DeepCopyNode(dst, src, oc);
                if (newChild < 0) return 0;
                Gon__StripSuffixesRecursive(dst, newChild);
                AddChild(dst, dstIdx, newChild);
                oc = src->nodes[oc].next_sibling;
            }
            return 1;
        }

        /* MERGE / APPEND / DEFAULT / ADD / MULTIPLY for OBJECT */
        {
            /* Duplicate-name book-keeping.  Matches the C++ logic of
             * tracking how many same-name children we've already visited
             * in 'self', so a patch with [foo, foo, foo] correctly
             * merges into three distinct existing foos instead of
             * merging all three into the first one. */
            struct NameCount {
                char name[GON_MAX_STRING_LEN];
                int  count;
            };
            struct NameCount counts[64];
            int nc = 0;

            oc = other->first_child;
            while (oc != -1)
            {
                const GonNode* oChild = &src->nodes[oc];
                const char* rawName = oChild->name;

                if (Gon__HasPatchSuffix(rawName))
                {
                    /* Per-child patch mode: strip suffix, find/insert
                     * by stripped name. */
                    char stripped[GON_MAX_STRING_LEN];
                    int i;
                    int myCountIdx;
                    int skipN;
                    int myChild;

                    strncpy(stripped, rawName, GON_MAX_STRING_LEN - 1);
                    stripped[GON_MAX_STRING_LEN - 1] = '\0';
                    Gon__StripPatchSuffix(stripped);

                    if (stripped[0] == '\0')
                    {
                        /* Child literally named ".append" / ".merge" /
                         * ".overwrite" etc. -per Glaiel's spec, this
                         * means "patch self with this child's contents"
                         * instead of inserting it. */
                        if (!Gon__PatchMergeIdx(dst, dstIdx, src, oc))
                            return 0;
                        oc = src->nodes[oc].next_sibling;
                        continue;
                    }

                    /* Find the per-name dedup counter. */
                    myCountIdx = -1;
                    for (i = 0; i < nc; i++)
                    {
                        if (strcmp(counts[i].name, stripped) == 0)
                        {
                            myCountIdx = i;
                            break;
                        }
                    }
                    if (myCountIdx < 0)
                    {
                        if (nc >= 64) return 0;       /* overflow */
                        strncpy(counts[nc].name, stripped,
                                GON_MAX_STRING_LEN - 1);
                        counts[nc].name[GON_MAX_STRING_LEN - 1] = '\0';
                        counts[nc].count = 0;
                        myCountIdx = nc++;
                    }
                    skipN = counts[myCountIdx].count;

                    myChild = Gon__NthChildWithNameIdx(
                        dst, dstIdx, stripped, skipN);

                    if (myChild >= 0)
                    {
                        if (!Gon__PatchMergeIdx(dst, myChild, src, oc))
                            return 0;
                        counts[myCountIdx].count++;
                    }
                    else
                    {
                        int newChild = Gon_DeepCopyNode(dst, src, oc);
                        if (newChild < 0) return 0;
                        Gon__StripSuffixesRecursive(dst, newChild);
                        /* The stripped node's name may still have the
                         * suffix if the suffix appeared only on the top
                         * node's name (not recursively).  Force it. */
                        strncpy(dst->nodes[newChild].name, stripped,
                                GON_MAX_STRING_LEN - 1);
                        dst->nodes[newChild].name[GON_MAX_STRING_LEN - 1] = '\0';
                        AddChild(dst, dstIdx, newChild);
                    }
                }
                else
                {
                    /* No per-child suffix -use the parent-level policy. */
                    if (policy == GON_MERGE_APPEND ||
                        policy == GON_MERGE_ADD)
                    {
                        int newChild = Gon_DeepCopyNode(dst, src, oc);
                        if (newChild < 0) return 0;
                        Gon__StripSuffixesRecursive(dst, newChild);
                        AddChild(dst, dstIdx, newChild);
                    }
                    else
                    {
                        /* MERGE / DEFAULT / MULTIPLY: recurse into
                         * matching-name child, else insert new. */
                        int i;
                        int myCountIdx = -1;
                        int skipN;
                        int myChild;

                        for (i = 0; i < nc; i++)
                        {
                            if (strcmp(counts[i].name, rawName) == 0)
                            {
                                myCountIdx = i;
                                break;
                            }
                        }
                        if (myCountIdx < 0)
                        {
                            if (nc >= 64) return 0;
                            strncpy(counts[nc].name, rawName,
                                    GON_MAX_STRING_LEN - 1);
                            counts[nc].name[GON_MAX_STRING_LEN - 1] = '\0';
                            counts[nc].count = 0;
                            myCountIdx = nc++;
                        }
                        skipN = counts[myCountIdx].count;

                        myChild = Gon__NthChildWithNameIdx(
                            dst, dstIdx, rawName, skipN);

                        if (myChild >= 0)
                        {
                            if (!Gon__PatchMergeIdx(dst, myChild, src, oc))
                                return 0;
                            counts[myCountIdx].count++;
                        }
                        else
                        {
                            int newChild = Gon_DeepCopyNode(dst, src, oc);
                            if (newChild < 0) return 0;
                            Gon__StripSuffixesRecursive(dst, newChild);
                            AddChild(dst, dstIdx, newChild);
                        }
                    }
                }

                oc = src->nodes[oc].next_sibling;
            }
        }
        return 1;
    }

    /* -- STRING + STRING -- */
    if (self->type == GON_TYPE_STRING && other->type == GON_TYPE_STRING)
    {
        if (policy == GON_MERGE_APPEND || policy == GON_MERGE_ADD)
        {
            size_t sl = strlen(self->str_val);
            size_t ol = strlen(other->str_val);
            if (sl + ol >= GON_MAX_STRING_LEN)
                ol = (GON_MAX_STRING_LEN - 1) - sl;
            memcpy(self->str_val + sl, other->str_val, ol);
            self->str_val[sl + ol] = '\0';
        }
        else
        {
            strncpy(self->str_val, other->str_val, GON_MAX_STRING_LEN - 1);
            self->str_val[GON_MAX_STRING_LEN - 1] = '\0';
        }
        return 1;
    }

    /* -- NUMBER + NUMBER -- */
    if (self->type == GON_TYPE_NUMBER && other->type == GON_TYPE_NUMBER)
    {
        if (policy == GON_MERGE_ADD)
        {
            self->dbl_val += other->dbl_val;
            self->int_val  = (int)self->dbl_val;
            snprintf(self->str_val, GON_MAX_STRING_LEN - 1,
                     "%g", self->dbl_val);
            self->str_val[GON_MAX_STRING_LEN - 1] = '\0';
        }
        else if (policy == GON_MERGE_MULTIPLY)
        {
            self->dbl_val *= other->dbl_val;
            self->int_val  = (int)self->dbl_val;
            snprintf(self->str_val, GON_MAX_STRING_LEN - 1,
                     "%g", self->dbl_val);
            self->str_val[GON_MAX_STRING_LEN - 1] = '\0';
        }
        else
        {
            self->dbl_val = other->dbl_val;
            self->int_val = other->int_val;
            strncpy(self->str_val, other->str_val, GON_MAX_STRING_LEN - 1);
            self->str_val[GON_MAX_STRING_LEN - 1] = '\0';
        }
        return 1;
    }

    /* -- NULL self: adopt other wholesale (preserve our name) -- */
    if (self->type == GON_TYPE_NULL)
    {
        char savedName[GON_MAX_STRING_LEN];
        int dupIdx;
        GonNode* dup;

        strcpy(savedName, self->name);
        dupIdx = Gon_DeepCopyNode(dst, src, srcIdx);
        if (dupIdx < 0) return 0;
        dup = &dst->nodes[dupIdx];

        /* Reseat dupIdx's fields onto self.  self pointer was taken
         * before dst->nodes may have been reallocated, but our pool is
         * a fixed array (nodes[]), so &dst->nodes[dstIdx] is stable. */
        self = &dst->nodes[dstIdx];
        self->type        = dup->type;
        memcpy(self->str_val, dup->str_val, GON_MAX_STRING_LEN);
        self->int_val     = dup->int_val;
        self->dbl_val     = dup->dbl_val;
        self->first_child = dup->first_child;
        self->child_count = dup->child_count;
        strcpy(self->name, savedName);
        Gon__StripPatchSuffix(self->name);
        /* dup node is orphaned (not linked as a child of anything);
         * harmless in a growing-only pool. */
        return 1;
    }

    /* -- Type mismatch: other overwrites self, name preserved -- */
    {
        char savedName[GON_MAX_STRING_LEN];
        int dupIdx;
        GonNode* dup;

        strcpy(savedName, self->name);
        dupIdx = Gon_DeepCopyNode(dst, src, srcIdx);
        if (dupIdx < 0) return 0;
        dup = &dst->nodes[dupIdx];

        self = &dst->nodes[dstIdx];
        self->type        = dup->type;
        memcpy(self->str_val, dup->str_val, GON_MAX_STRING_LEN);
        self->int_val     = dup->int_val;
        self->dbl_val     = dup->dbl_val;
        self->first_child = dup->first_child;
        self->child_count = dup->child_count;
        strcpy(self->name, savedName);
        Gon__StripPatchSuffix(self->name);
        Gon__StripSuffixesRecursive(dst, dstIdx);
        /* Restore our name one more time (StripSuffixesRecursive may
         * have touched it). */
        strcpy(self->name, savedName);
        Gon__StripPatchSuffix(self->name);
    }
    return 1;
}


int Gon_PatchMergeDoc(GonDoc* dstDoc, const GonDoc* srcDoc)
{
    if (!dstDoc || !srcDoc)        return 0;
    if (!dstDoc->parse_ok)         return 0;
    if (!srcDoc->parse_ok)         return 0;
    if (dstDoc->node_count == 0)   return 0;
    if (srcDoc->node_count == 0)   return 0;
    return Gon__PatchMergeIdx(dstDoc, 0, srcDoc, 0);
}

/* See runtime/include/igi_menu_patch.h.
 *
 * This mirrors tools/vita_menu/build_menu.py and tools/qvm/qvm.py in C: it
 * decodes a "LOOP" version-8 compiled menu script into a small node tree, adds
 * the Vita options screen (id 980) and the "Vita" entry, and re-encodes it. The
 * encoding reproduces qvm.py's _Encoder byte for byte (same identifier/string
 * table order with new entries appended, same sized opcodes), so the patched
 * file is identical to the one the Python tool produces. A parse failure or an
 * unexpected layout leaves the file untouched; the original is kept as
 * <file>.orig the first time, so a second run rebuilds from it and is a no-op
 * once the screen is present (idempotent), like igi_controls.c's backup_config.
 *
 * The menu scripts use only a tiny declarative subset of the byte code: every
 * statement is a call Name(arg, ...) followed by POP; the program ends with
 * BRK. Arguments are strings, integers, bare identifiers (TRUE/FALSE) or nested
 * calls. That subset is all this module decodes and encodes. */
#include "igi_menu_patch.h"

#include "runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- byte code opcodes (the order is the interpreter's, see qvm.py) ---- */
enum {
    OP_BRK = 0, OP_NOP, OP_PUSH, OP_PUSHB, OP_PUSHW, OP_PUSHF, OP_PUSHA, OP_PUSHS,
    OP_PUSHSI, OP_PUSHSIB, OP_PUSHSIW, OP_PUSHI, OP_PUSHII, OP_PUSHIIB, OP_PUSHIIW,
    OP_PUSH0, OP_PUSH1, OP_PUSHM, OP_POP, OP_RET, OP_BRA, OP_BF, OP_BT, OP_JSR,
    OP_CALL, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SHL, OP_SHR, OP_AND, OP_OR, OP_XOR,
    OP_LAND, OP_LOR, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_ASSIGN, OP_PLUS,
    OP_MINUS, OP_INV, OP_NOT, OP_BLK
};

/* ---- node model ---------------------------------------------------------- */

typedef enum NodeKind { N_CALL, N_STR, N_INT, N_IDENT, N_NEG } NodeKind;

typedef struct Node {
    NodeKind kind;
    /* N_CALL: name + args; N_STR/N_IDENT: text; N_INT: ival; N_NEG: args[0]. */
    const char *name;       /* call name or identifier text; interned/owned string */
    const char *text;       /* string literal value (owned) */
    int64_t ival;
    struct Node **args;
    int nargs, argcap;
} Node;

/* All nodes and strings are allocated from an arena freed at the end, so the
 * builders stay allocation-check free. */
typedef struct Arena {
    void **blocks;
    int n, cap;
} Arena;

static void *arena_alloc(Arena *a, size_t size)
{
    void *p = calloc(1, size ? size : 1);
    if (!p)
        return NULL;
    if (a->n == a->cap) {
        int cap = a->cap ? a->cap * 2 : 64;
        void **g = realloc(a->blocks, (size_t)cap * sizeof(*g));
        if (!g) {
            free(p);
            return NULL;
        }
        a->blocks = g;
        a->cap = cap;
    }
    a->blocks[a->n++] = p;
    return p;
}

static void arena_free(Arena *a)
{
    for (int i = 0; i < a->n; i++)
        free(a->blocks[i]);
    free(a->blocks);
    a->blocks = NULL;
    a->n = a->cap = 0;
}

static char *arena_strndup(Arena *a, const char *s, size_t n)
{
    char *p = arena_alloc(a, n + 1);
    if (p)
        memcpy(p, s, n);
    return p;
}

static char *arena_strdup(Arena *a, const char *s)
{
    return arena_strndup(a, s, strlen(s));
}

/* A parse/build aborts by setting this; every helper then returns a null/empty
 * value so the first failure unwinds without touching the file. */
typedef struct Ctxp {
    Arena *a;
    int failed;
} Ctxp;

static void fail(Ctxp *x)
{
    x->failed = 1;
}

static Node *node_new(Ctxp *x, NodeKind kind)
{
    Node *n = arena_alloc(x->a, sizeof(*n));
    if (!n) {
        fail(x);
        return NULL;
    }
    n->kind = kind;
    return n;
}

static Node *node_int(Ctxp *x, int64_t v)
{
    Node *n = node_new(x, N_INT);
    if (n)
        n->ival = v;
    return n;
}

static Node *node_str(Ctxp *x, const char *s)
{
    Node *n = node_new(x, N_STR);
    if (n) {
        n->text = arena_strdup(x->a, s);
        if (!n->text)
            fail(x);
    }
    return n;
}

static Node *node_ident(Ctxp *x, const char *s)
{
    Node *n = node_new(x, N_IDENT);
    if (n) {
        n->name = arena_strdup(x->a, s);
        if (!n->name)
            fail(x);
    }
    return n;
}

static void node_add(Ctxp *x, Node *call, Node *kid)
{
    if (x->failed || !call || !kid)
        return;
    if (call->nargs == call->argcap) {
        int cap = call->argcap ? call->argcap * 2 : 8;
        Node **g = realloc(call->args, (size_t)cap * sizeof(*g));
        if (!g) {
            fail(x);
            return;
        }
        /* The arena frees node and string bodies; args arrays are freed here. */
        call->args = g;
        call->argcap = cap;
    }
    call->args[call->nargs++] = kid;
}

/* A call node with a variable argument list of (Node *), terminated by NULL. */
static Node *node_call(Ctxp *x, const char *name, ...)
{
    Node *n = node_new(x, N_CALL);
    if (!n)
        return NULL;
    n->name = arena_strdup(x->a, name);
    if (!n->name) {
        fail(x);
        return n;
    }
    va_list ap;
    va_start(ap, name);
    for (;;) {
        Node *kid = va_arg(ap, Node *);
        if (!kid)
            break;
        node_add(x, n, kid);
    }
    va_end(ap);
    return n;
}

/* args arrays live outside the arena (they are realloc'd), so free them with a
 * post-order walk once the tree is done. */
static void node_free_args(Node *n)
{
    if (!n || n->kind != N_CALL)
        return;
    for (int i = 0; i < n->nargs; i++)
        node_free_args(n->args[i]);
    free(n->args);
    n->args = NULL;
}

/* ---- decoder (a faithful subset of qvm.py's _Decoder) -------------------- */

typedef struct Script {
    Node **stmts;
    int nstmts, stmtcap;
    char **ids;         /* identifier table, original order */
    int nids;
    char **strs;        /* string table, original order */
    int nstrs;
    uint32_t version, minver, stack, calls;
} Script;

typedef struct Dec {
    Ctxp *x;
    const uint8_t *code;
    uint32_t clen;
    char **ids;
    int nids;
    char **strs;
    int nstrs;
} Dec;

static uint32_t rd_u(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static Node *dec_expr(Dec *d, uint32_t *pcp);

/* An expression possibly followed by unary MINUS (qvm.py operand()). */
static Node *dec_operand(Dec *d, uint32_t *pcp)
{
    Node *node = dec_expr(d, pcp);
    while (!d->x->failed && *pcp < d->clen && d->code[*pcp] == OP_MINUS) {
        Node *neg = node_new(d->x, N_NEG);
        if (!neg)
            return node;
        neg->args = malloc(sizeof(Node *));
        if (!neg->args) {
            fail(d->x);
            return node;
        }
        neg->args[0] = node;
        neg->nargs = neg->argcap = 1;
        node = neg;
        (*pcp)++;
    }
    return node;
}

static Node *dec_expr(Dec *d, uint32_t *pcp)
{
    Ctxp *x = d->x;
    if (x->failed || *pcp >= d->clen) {
        fail(x);
        return NULL;
    }
    uint32_t pc = *pcp;
    uint8_t op = d->code[pc++];
    if (op == OP_PUSHIIB || op == OP_PUSHIIW || op == OP_PUSHII) {
        int w = op == OP_PUSHIIB ? 1 : op == OP_PUSHIIW ? 2 : 4;
        if (pc + (uint32_t)w > d->clen) {
            fail(x);
            return NULL;
        }
        uint32_t idx = rd_u(d->code + pc, w);
        pc += (uint32_t)w;
        if (pc >= d->clen || d->code[pc] != OP_CALL) {
            /* a bare identifier operand (e.g. TRUE) */
            if ((int)idx >= d->nids) {
                fail(x);
                return NULL;
            }
            *pcp = pc;
            return node_ident(x, d->ids[idx]);
        }
        /* a call */
        pc++;   /* OP_CALL */
        if (pc + 4 > d->clen) {
            fail(x);
            return NULL;
        }
        uint32_t narg = rd_u(d->code + pc, 4);
        pc += 4;
        if (narg > 0x10000u || pc + 4u * narg > d->clen) {
            fail(x);
            return NULL;
        }
        uint32_t *offs = narg ? malloc(narg * sizeof(uint32_t)) : NULL;
        if (narg && !offs) {
            fail(x);
            return NULL;
        }
        for (uint32_t i = 0; i < narg; i++) {
            offs[i] = rd_u(d->code + pc, 4);
            pc += 4;
        }
        if (pc >= d->clen || d->code[pc] != OP_BRA) {
            free(offs);
            fail(x);
            return NULL;
        }
        pc++;
        if (pc + 4 > d->clen) {
            free(offs);
            fail(x);
            return NULL;
        }
        uint32_t skip = rd_u(d->code + pc, 4);
        pc += 4;
        uint32_t end = pc + skip;
        if ((int)idx >= d->nids) {
            free(offs);
            fail(x);
            return NULL;
        }
        Node *call = node_new(x, N_CALL);
        if (!call) {
            free(offs);
            return NULL;
        }
        call->name = d->ids[idx];    /* owned by the id table; copied on build */
        for (uint32_t i = 0; i < narg && !x->failed; i++) {
            if (offs[i] != pc) {
                free(offs);
                fail(x);
                return NULL;
            }
            Node *arg = dec_operand(d, &pc);
            if (x->failed || pc >= d->clen || d->code[pc] != OP_BRK) {
                free(offs);
                fail(x);
                return NULL;
            }
            pc++;
            node_add(x, call, arg);
        }
        free(offs);
        if (!x->failed && pc != end) {
            fail(x);
            return NULL;
        }
        *pcp = pc;
        return call;
    }
    if (op == OP_PUSHSIB || op == OP_PUSHSIW || op == OP_PUSHSI) {
        int w = op == OP_PUSHSIB ? 1 : op == OP_PUSHSIW ? 2 : 4;
        if (pc + (uint32_t)w > d->clen) {
            fail(x);
            return NULL;
        }
        uint32_t idx = rd_u(d->code + pc, w);
        pc += (uint32_t)w;
        if ((int)idx >= d->nstrs) {
            fail(x);
            return NULL;
        }
        *pcp = pc;
        return node_str(x, d->strs[idx]);
    }
    if (op == OP_PUSH0) {
        *pcp = pc;
        return node_int(x, 0);
    }
    if (op == OP_PUSH1) {
        *pcp = pc;
        return node_int(x, 1);
    }
    if (op == OP_PUSHB || op == OP_PUSHW || op == OP_PUSH) {
        int w = op == OP_PUSHB ? 1 : op == OP_PUSHW ? 2 : 4;
        if (pc + (uint32_t)w > d->clen) {
            fail(x);
            return NULL;
        }
        int64_t v;
        if (w == 1)
            v = (int8_t)d->code[pc];
        else if (w == 2)
            v = (int16_t)rd_u(d->code + pc, 2);
        else
            v = (int32_t)rd_u(d->code + pc, 4);
        pc += (uint32_t)w;
        *pcp = pc;
        return node_int(x, v);
    }
    /* PUSHF and anything else: the menu subset has no floats; treat as failure
     * so an unexpected script is left untouched rather than mis-encoded. */
    fail(x);
    return NULL;
}

static char **read_table(Ctxp *x, const uint8_t *blob, uint32_t len, uint32_t toff,
                         uint32_t doff, uint32_t tsize, int *count)
{
    uint32_t n = tsize / 4;
    *count = (int)n;
    if (n == 0)
        return NULL;
    char **out = arena_alloc(x->a, n * sizeof(char *));
    if (!out) {
        fail(x);
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (toff + 4 * i + 4 > len) {
            fail(x);
            return NULL;
        }
        uint32_t o = rd_u(blob + toff + 4 * i, 4);
        uint32_t start = doff + o;
        if (start > len) {
            fail(x);
            return NULL;
        }
        uint32_t e = start;
        while (e < len && blob[e] != 0)
            e++;
        if (e >= len) {
            fail(x);
            return NULL;
        }
        out[i] = arena_strndup(x->a, (const char *)blob + start, e - start);
        if (!out[i]) {
            fail(x);
            return NULL;
        }
    }
    return out;
}

/* Parse a whole script (qvm.py parse()). Returns 0 on failure. */
static int script_parse(Ctxp *x, const uint8_t *blob, uint32_t len, Script *s)
{
    memset(s, 0, sizeof(*s));
    if (len < 0x3C || memcmp(blob, "LOOP", 4) != 0)
        return 0;
    uint32_t h[14];
    for (int i = 0; i < 14; i++)
        h[i] = rd_u(blob + 4 + 4 * i, 4);
    uint32_t version = h[0], minver = h[1];
    uint32_t idt = h[2], idd = h[3], idts = h[4];
    uint32_t st = h[6], sd = h[7], sts = h[8];
    uint32_t co = h[10], cs = h[11], stack = h[12], calls = h[13];
    if (version != 8)
        return 0;
    s->version = version;
    s->minver = minver;
    s->stack = stack;
    s->calls = calls;
    s->ids = read_table(x, blob, len, idt, idd, idts, &s->nids);
    s->strs = read_table(x, blob, len, st, sd, sts, &s->nstrs);
    if (x->failed)
        return 0;
    if (co > len || cs > len || co + cs > len)
        return 0;
    Dec d = { x, blob + co, cs, s->ids, s->nids, s->strs, s->nstrs };
    uint32_t pc = 0;
    while (pc < cs) {
        if (d.code[pc] == OP_BRK && pc == cs - 1)
            break;   /* the program ends with BRK */
        Node *node = dec_operand(&d, &pc);
        if (x->failed || pc >= cs || d.code[pc] != OP_POP)
            return 0;
        pc++;
        if (s->nstmts == s->stmtcap) {
            int cap = s->stmtcap ? s->stmtcap * 2 : 64;
            Node **g = realloc(s->stmts, (size_t)cap * sizeof(*g));
            if (!g)
                return 0;
            s->stmts = g;
            s->stmtcap = cap;
        }
        s->stmts[s->nstmts++] = node;
    }
    return !x->failed;
}

/* ---- encoder (a faithful port of qvm.py's _Encoder + build) -------------- */

typedef struct Tab {
    char **items;
    int n, cap;
    /* first-seen index per item; a linear scan is fine for these small tables. */
} Tab;

static void tab_seed(Ctxp *x, Tab *t, char **items, int n)
{
    t->cap = n + 64;
    t->items = malloc((size_t)t->cap * sizeof(char *));
    if (!t->items) {
        fail(x);
        return;
    }
    for (int i = 0; i < n; i++)
        t->items[i] = items[i];
    t->n = n;
}

/* Index of s in the table (first occurrence), appending it if absent. Mirrors
 * qvm.py intern(): the reversed-enumerate seed means the FIRST occurrence wins,
 * which a forward scan from 0 reproduces. */
static int tab_intern(Ctxp *x, Tab *t, const char *s)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->items[i], s) == 0)
            return i;
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        char **g = realloc(t->items, (size_t)cap * sizeof(char *));
        if (!g) {
            fail(x);
            return 0;
        }
        t->items = g;
        t->cap = cap;
    }
    t->items[t->n] = (char *)s;
    return t->n++;
}

typedef struct Buf {
    uint8_t *data;
    uint32_t len, cap;
    int failed;
} Buf;

static void buf_put(Buf *b, const void *p, uint32_t n)
{
    if (b->failed)
        return;
    if (b->len + n > b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *g = realloc(b->data, cap);
        if (!g) {
            b->failed = 1;
            return;
        }
        b->data = g;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void buf_u8(Buf *b, uint8_t v) { buf_put(b, &v, 1); }

static void buf_u32(Buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    buf_put(b, t, 4);
}

/* qvm.py sized(): choose PUSHB/W or PUSHSIB/W... by magnitude. For ints the
 * callers only ever pass non-negative values here (negatives go through Neg). */
static void enc_sized(Buf *b, int op_b, int op_w, int op_d, uint32_t v, int is_signed)
{
    if (is_signed) {
        if (v < 0x80u) {
            buf_u8(b, (uint8_t)op_b);
            buf_u8(b, (uint8_t)v);
        } else if (v < 0x8000u) {
            buf_u8(b, (uint8_t)op_w);
            buf_u8(b, (uint8_t)v);
            buf_u8(b, (uint8_t)(v >> 8));
        } else {
            buf_u8(b, (uint8_t)op_d);
            buf_u32(b, v);
        }
    } else {
        if (v < 0x100u) {
            buf_u8(b, (uint8_t)op_b);
            buf_u8(b, (uint8_t)v);
        } else if (v < 0x10000u) {
            buf_u8(b, (uint8_t)op_w);
            buf_u8(b, (uint8_t)v);
            buf_u8(b, (uint8_t)(v >> 8));
        } else {
            buf_u8(b, (uint8_t)op_d);
            buf_u32(b, v);
        }
    }
}

typedef struct Enc {
    Ctxp *x;
    Tab ids;
    Tab strs;
} Enc;

/* Encode node whose code starts at `base` (qvm.py _Encoder.expr). */
static void enc_expr(Enc *e, const Node *node, uint32_t base, Buf *out);

static void enc_expr(Enc *e, const Node *node, uint32_t base, Buf *out)
{
    Ctxp *x = e->x;
    if (x->failed || out->failed || !node)
        return;
    switch (node->kind) {
    case N_CALL: {
        int idx = tab_intern(x, &e->ids, node->name);
        Buf head = { 0 };
        enc_sized(&head, OP_PUSHIIB, OP_PUSHIIW, OP_PUSHII, (uint32_t)idx, 0);
        uint32_t n = (uint32_t)node->nargs;
        uint32_t args_start = base + head.len + 5 + 4 * n + 5;
        Buf body = { 0 };
        uint32_t *offs = n ? malloc(n * sizeof(uint32_t)) : NULL;
        if (n && !offs) {
            fail(x);
            free(head.data);
            return;
        }
        for (uint32_t i = 0; i < n; i++) {
            offs[i] = args_start + body.len;
            enc_expr(e, node->args[i], args_start + body.len, &body);
            buf_u8(&body, OP_BRK);
        }
        /* head */
        buf_put(out, head.data, head.len);
        buf_u8(out, OP_CALL);
        buf_u32(out, n);
        for (uint32_t i = 0; i < n; i++)
            buf_u32(out, offs[i]);
        buf_u8(out, OP_BRA);
        buf_u32(out, body.len);
        buf_put(out, body.data, body.len);
        if (head.failed || body.failed)
            out->failed = 1;
        free(head.data);
        free(body.data);
        free(offs);
        break;
    }
    case N_IDENT: {
        int idx = tab_intern(x, &e->ids, node->name);
        enc_sized(out, OP_PUSHIIB, OP_PUSHIIW, OP_PUSHII, (uint32_t)idx, 0);
        break;
    }
    case N_NEG:
        enc_expr(e, node->args[0], base, out);
        buf_u8(out, OP_MINUS);
        break;
    case N_STR: {
        int idx = tab_intern(x, &e->strs, node->text);
        enc_sized(out, OP_PUSHSIB, OP_PUSHSIW, OP_PUSHSI, (uint32_t)idx, 0);
        break;
    }
    case N_INT: {
        int64_t v = node->ival;
        if (v < 0) {
            Node neg = { 0 }, inner = { 0 };
            Node *kids[1] = { &inner };
            inner.kind = N_INT;
            inner.ival = -v;
            neg.kind = N_NEG;
            neg.args = kids;
            neg.nargs = 1;
            enc_expr(e, &neg, base, out);
        } else if (v == 0) {
            buf_u8(out, OP_PUSH0);
        } else if (v == 1) {
            buf_u8(out, OP_PUSH1);
        } else {
            enc_sized(out, OP_PUSHB, OP_PUSHW, OP_PUSH, (uint32_t)v, 1);
        }
        break;
    }
    }
}

/* qvm.py _pack_table: cp1252 is pass-through for the ASCII/Latin-1 bytes these
 * strings use; the C source already holds them as the exact byte values. */
static void pack_table(Buf *offs, Buf *data, char **items, int n)
{
    for (int i = 0; i < n; i++) {
        buf_u32(offs, data->len);
        buf_put(data, items[i], (uint32_t)strlen(items[i]) + 1);
    }
}

/* Encode a whole script (qvm.py build()). Returns a malloc'd buffer; *outlen set.
 * The caller frees it. NULL on failure. */
static uint8_t *script_build(Ctxp *x, const Script *s, uint32_t *outlen)
{
    Enc e = { x, { 0 }, { 0 } };
    tab_seed(x, &e.ids, s->ids, s->nids);
    tab_seed(x, &e.strs, s->strs, s->nstrs);
    if (x->failed) {
        free(e.ids.items);
        free(e.strs.items);
        return NULL;
    }
    Buf code = { 0 };
    for (int i = 0; i < s->nstmts && !x->failed; i++) {
        enc_expr(&e, s->stmts[i], code.len, &code);
        buf_u8(&code, OP_POP);
    }
    buf_u8(&code, OP_BRK);

    Buf idt = { 0 }, idd = { 0 }, stt = { 0 }, sdd = { 0 };
    pack_table(&idt, &idd, e.ids.items, e.ids.n);
    pack_table(&stt, &sdd, e.strs.items, e.strs.n);

    if (x->failed || code.failed || idt.failed || idd.failed || stt.failed || sdd.failed) {
        free(e.ids.items);
        free(e.strs.items);
        free(code.data);
        free(idt.data);
        free(idd.data);
        free(stt.data);
        free(sdd.data);
        return NULL;
    }

    uint32_t pos = 0x3C;   /* HEADER.size: 4 + 14*4 */
    uint32_t off_idt = pos; pos += idt.len;
    uint32_t off_idd = pos; pos += idd.len;
    uint32_t off_stt = pos; pos += stt.len;
    uint32_t off_sdd = pos; pos += sdd.len;
    uint32_t off_code = pos; pos += code.len;

    Buf out = { 0 };
    buf_put(&out, "LOOP", 4);
    buf_u32(&out, s->version);
    buf_u32(&out, s->minver);
    buf_u32(&out, off_idt);
    buf_u32(&out, off_idd);
    buf_u32(&out, idt.len);
    buf_u32(&out, idd.len);
    buf_u32(&out, off_stt);
    buf_u32(&out, off_sdd);
    buf_u32(&out, stt.len);
    buf_u32(&out, sdd.len);
    buf_u32(&out, off_code);
    buf_u32(&out, code.len);
    buf_u32(&out, s->stack);
    buf_u32(&out, s->calls);
    buf_put(&out, idt.data, idt.len);
    buf_put(&out, idd.data, idd.len);
    buf_put(&out, stt.data, stt.len);
    buf_put(&out, sdd.data, sdd.len);
    buf_put(&out, code.data, code.len);

    free(e.ids.items);
    free(e.strs.items);
    free(code.data);
    free(idt.data);
    free(idd.data);
    free(stt.data);
    free(sdd.data);

    if (out.failed) {
        free(out.data);
        return NULL;
    }
    *outlen = out.len;
    return out.data;
}

/* ---- the menu edit (mirrors build_menu.py) ------------------------------- */

/* The rows of the Vita screen, in order. kind 0 = list, 1 = toggle. */
static const struct { int toggle; const char *key; const char *fn; } ROWS[] = {
    { 0, "Aspect", "Aspect" },
    { 0, "TouchAim", "TouchAim" },
    { 1, "Buttons", "Buttons" },
    { 0, "Rear", "Rear" },
    { 1, "FrontTouch", "FrontTouch" },
    { 0, "Sensitivity", "Sensitivity" },
    { 1, "Overlay", "Overlay" },
    { 1, "UpdateCheck", "UpdateCheck" },
};
#define N_ROWS ((int)(sizeof(ROWS) / sizeof(ROWS[0])))
#define VITA_SCREEN 980
#define FIRST_WIDGET 980

static Node *str_(Ctxp *x, const char *s) { return node_str(x, s); }
static Node *int_(Ctxp *x, int64_t v) { return node_int(x, v); }
static Node *ident_(Ctxp *x, const char *s) { return node_ident(x, s); }

/* snprintf into an arena string. */
static const char *afmt(Ctxp *x, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(tmp)) {
        fail(x);
        return "";
    }
    const char *s = arena_strdup(x->a, tmp);
    if (!s) {
        fail(x);
        return "";
    }
    return s;
}

/* MenuFrame(id=-1, "MenuFrame", name, "", x, y, w, h, lineup, xsp, ysp, sel, kids...). */
static Node *menuframe(Ctxp *x, const char *name, int X, int Y, int W, int H,
                       int lineup, int xsp, int ysp, int sel, Node **kids, int nkids)
{
    Node *n = node_call(x, "Task_New",
                        int_(x, -1), str_(x, "MenuFrame"), str_(x, name), str_(x, ""),
                        int_(x, X), int_(x, Y), int_(x, W), int_(x, H),
                        int_(x, lineup), int_(x, xsp), int_(x, ysp),
                        ident_(x, sel ? "TRUE" : "FALSE"), (Node *)NULL);
    for (int i = 0; i < nkids; i++)
        node_add(x, n, kids[i]);
    return n;
}

/* MenuText(id=-1, "MenuText", internalName, TextKey, Font, Colour, Click, Sound). */
static Node *menutext(Ctxp *x, const char *internal, const char *key, const char *font,
                      int colour, const char *click, const char *sound)
{
    return node_call(x, "Task_New",
                     int_(x, -1), str_(x, "MenuText"), str_(x, internal), str_(x, key),
                     str_(x, font), int_(x, colour), str_(x, click), str_(x, sound),
                     (Node *)NULL);
}

/* A label + widget row, matching build_menu.py's row(). title_key already has
 * the "Vita_" prefix. */
static Node *row(Ctxp *x, int toggle, const char *title_key, const char *fn, int wid, int y)
{
    Node *label = menutext(x, "", title_key, "font3.fnt", 2, "", "");
    Node *widget;
    if (toggle) {
        widget = node_call(x, "Task_New",
                           int_(x, wid), str_(x, "ToggleBox"), str_(x, ""),
                           str_(x, afmt(x, "Vita_Get%s()", fn)),
                           str_(x, afmt(x, "Vita_Set%s(this.isOn)", fn)), (Node *)NULL);
    } else {
        widget = node_call(x, "Task_New",
                           int_(x, wid), str_(x, "ListBox"), str_(x, ""), str_(x, "slide"),
                           str_(x, "font1.fnt"), int_(x, 150),
                           str_(x, afmt(x, "Vita_Fill%s()", fn)),
                           str_(x, afmt(x, "Vita_Get%s()", fn)),
                           str_(x, ""),
                           str_(x, afmt(x, "Vita_Set%s(this.nActiveID)", fn)), (Node *)NULL);
    }
    Node *kids[2] = { label, widget };
    return menuframe(x, title_key, 95, y, 454, 25, 8, 40, 4, 1, kids, 2);
}

/* The MenuScreen 980 with every setting row (build_menu.py vita_screen()). */
static Node *vita_screen(Ctxp *x, const char *commit_action)
{
    Node *tkid = menutext(x, "", "Vita Settings", "font3.fnt", 1, "", "");
    Node *title = menuframe(x, "", 95, 161, 455, 26, 0, 4, 4, 0, &tkid, 1);

    Node *rows[N_ROWS];
    int y = 189, wid = FIRST_WIDGET;
    for (int i = 0; i < N_ROWS; i++) {
        rows[i] = row(x, ROWS[i].toggle, afmt(x, "Vita_%s", ROWS[i].key), ROWS[i].fn, wid, y);
        y += 27;
        wid += 1;
    }
    Node *rows_frame = menuframe(x, "", 95, 185, 455, 26, 1, 4, 2, 0, rows, N_ROWS);

    Node *okkid = menutext(x, "", "OK", "font3.fnt", 2, commit_action, "menu_ok");
    Node *okcancel = menuframe(x, "OK/Cancel", 555, 400, 80, 80, 1, 4, 4, 0, &okkid, 1);

    Node *screen = node_call(x, "Task_New",
                             int_(x, VITA_SCREEN), str_(x, "MenuScreen"), str_(x, "Vita Settings"),
                             str_(x, "mainmenu.pic"), int_(x, 100), int_(x, 163), int_(x, 540),
                             int_(x, 470), str_(x, commit_action), str_(x, ""), str_(x, ""),
                             (Node *)NULL);
    node_add(x, screen, title);
    node_add(x, screen, rows_frame);
    node_add(x, screen, okcancel);
    return screen;
}

static int node_is_call(const Node *n, const char *name)
{
    return n && n->kind == N_CALL && strcmp(n->name, name) == 0;
}

static int arg_is_str(const Node *n, int i, const char *val)
{
    return n && i < n->nargs && n->args[i]->kind == N_STR && strcmp(n->args[i]->text, val) == 0;
}

static int arg_is_int(const Node *n, int i, int64_t v)
{
    return n && i < n->nargs && n->args[i]->kind == N_INT && n->args[i]->ival == v;
}

/* Walk every call in a tree looking for a MenuScreen Task_New with id == sid. */
static const Node *find_screen(const Node *n, int sid)
{
    if (!n || n->kind != N_CALL)
        return NULL;
    if (node_is_call(n, "Task_New") && arg_is_int(n, 0, sid) && arg_is_str(n, 1, "MenuScreen"))
        return n;
    for (int i = 0; i < n->nargs; i++) {
        const Node *r = find_screen(n->args[i], sid);
        if (r)
            return r;
    }
    return NULL;
}

static const Node *script_find_screen(const Script *s, int sid)
{
    for (int i = 0; i < s->nstmts; i++) {
        const Node *r = find_screen(s->stmts[i], sid);
        if (r)
            return r;
    }
    return NULL;
}

/* The top-level MenuManager call (build_menu.py manager_of()). */
static Node *script_manager(const Script *s)
{
    for (int i = 0; i < s->nstmts; i++)
        if (node_is_call(s->stmts[i], "Task_New") && arg_is_str(s->stmts[i], 1, "MenuManager"))
            return s->stmts[i];
    return NULL;
}

/* The list frame of an Options screen (build_menu.py rows_frame_of()): the last
 * MenuFrame kid with >= 2 MenuText/MenuTextConditional kids. */
static Node *rows_frame_of(const Node *screen)
{
    Node *best = NULL;
    for (int i = 0; i < screen->nargs; i++) {
        Node *a = screen->args[i];
        if (!node_is_call(a, "Task_New") || !arg_is_str(a, 1, "MenuFrame"))
            continue;
        int texts = 0;
        for (int k = 0; k < a->nargs; k++) {
            Node *kid = a->args[k];
            if (node_is_call(kid, "Task_New") && kid->nargs > 1 && kid->args[1]->kind == N_STR &&
                (strcmp(kid->args[1]->text, "MenuText") == 0 ||
                 strcmp(kid->args[1]->text, "MenuTextConditional") == 0))
                texts++;
        }
        if (texts >= 2)
            best = a;
    }
    return best;
}

/* Add a 'Vita' MenuText entry to a row frame if not already present (keyed on
 * the Click script at arg index 6), build_menu.py add_entry(). */
static void add_entry(Ctxp *x, Node *rows_frame, const char *click)
{
    for (int i = 0; i < rows_frame->nargs; i++) {
        Node *kid = rows_frame->args[i];
        if (node_is_call(kid, "Task_New") && kid->nargs > 6 && arg_is_str(kid, 6, click))
            return;   /* already there */
    }
    Node *entry = menutext(x, "Vita", "Vita", "font3.fnt", 2, click, "");
    node_add(x, rows_frame, entry);
}

/* Patch a parsed script. is_main selects the main-menu vs pause-menu wording.
 * Returns 0 if the expected layout is absent (leave the file untouched). */
static int patch_script(Ctxp *x, Script *s, int is_main)
{
    int opt_id = is_main ? 904 : 900;
    const Node *opt = script_find_screen(s, opt_id);
    if (!opt)
        return 0;
    Node *frame = rows_frame_of(opt);
    if (!frame)
        return 0;
    Node *mgr = script_manager(s);
    if (!mgr)
        return 0;

    const char *entry_click = is_main ? afmt(x, "MenuManager_RequestScreen(%d, FALSE)", VITA_SCREEN)
                                      : afmt(x, "MenuManager_PushScreen(%d)\n", VITA_SCREEN);
    add_entry(x, frame, entry_click);

    if (!script_find_screen(s, VITA_SCREEN)) {
        const char *commit = is_main ? "MenuManager_RequestScreen(904, TRUE)"
                                     : "MenuManager_PopScreen(TRUE)";
        Node *screen = vita_screen(x, commit);
        node_add(x, mgr, screen);
    }
    return !x->failed;
}

/* ---- file driver --------------------------------------------------------- */

static uint8_t *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 64 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *len = (uint32_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, uint32_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    size_t put = fwrite(data, 1, len, f);
    int ok = fclose(f) == 0 && put == len;
    return ok;
}

/* Free the parts of a Script that live outside the arena. */
static void script_dispose(Script *s)
{
    for (int i = 0; i < s->nstmts; i++)
        node_free_args(s->stmts[i]);
    free(s->stmts);
    s->stmts = NULL;
}

/* Patch one menu file in place, keeping <file>.orig the first time, from its
 * unmodified original; idempotent once screen 980 is present. */
static void patch_file(const char *name, int is_main)
{
    char path[1200], orig[1216];
    if ((size_t)snprintf(path, sizeof(path), "%s/menusystem/%s", g_game_dir, name) >= sizeof(path))
        return;
    if ((size_t)snprintf(orig, sizeof(orig), "%s.orig", path) >= sizeof(orig))
        return;

    /* The live file: if it already carries the Vita screen the patch is done,
     * so leave it (and its backup) untouched -- the menu file is written at most
     * once, like igi_controls.c's backup_config, not on every launch. */
    uint32_t live_len = 0;
    uint8_t *live = read_file(path, &live_len);
    if (!live)
        return;   /* no such menu file */
    {
        Arena a0 = { 0 };
        Ctxp x0 = { &a0, 0 };
        Script s0;
        int done_already = script_parse(&x0, live, live_len, &s0) && !x0.failed &&
                           script_find_screen(&s0, VITA_SCREEN);
        script_dispose(&s0);
        arena_free(&a0);
        if (done_already) {
            free(live);
            return;
        }
    }

    /* Patch from the kept unmodified original if present, else the live file
     * (first run); the live bytes become the backup below. */
    uint32_t len = 0;
    uint8_t *blob = read_file(orig, &len);
    int have_orig = blob != NULL;
    if (!blob) {
        blob = live;
        len = live_len;
        live = NULL;
    }
    free(live);

    Arena arena = { 0 };
    Ctxp x = { &arena, 0 };
    Script s;
    int parsed = script_parse(&x, blob, len, &s);

    if (!parsed || x.failed) {
        rt_log("menu patch: %s: cannot parse, left untouched", name);
        script_dispose(&s);
        arena_free(&arena);
        free(blob);
        return;
    }

    /* The source still carries the screen (a patched file with no .orig): keep
     * it as-is and do not create a late backup. */
    if (script_find_screen(&s, VITA_SCREEN)) {
        script_dispose(&s);
        arena_free(&arena);
        free(blob);
        return;
    }

    if (!patch_script(&x, &s, is_main) || x.failed) {
        rt_log("menu patch: %s: unexpected layout, left untouched", name);
        script_dispose(&s);
        arena_free(&arena);
        free(blob);
        return;
    }

    uint32_t outlen = 0;
    uint8_t *out = script_build(&x, &s, &outlen);
    script_dispose(&s);
    arena_free(&arena);
    if (!out || x.failed) {
        rt_log("menu patch: %s: re-encode failed, left untouched", name);
        free(out);
        free(blob);
        return;
    }

    /* Keep the unmodified original the first time (from the live bytes we read). */
    if (!have_orig && !write_file(orig, blob, len)) {
        rt_log("menu patch: %s: cannot write backup, left untouched", name);
        free(out);
        free(blob);
        return;
    }
    free(blob);

    if (!write_file(path, out, outlen))
        rt_log("menu patch: %s: cannot write patched menu", name);
    else
        rt_log("menu patch: %s: added the Vita options screen", name);
    free(out);
}

void igi_menu_patch_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    patch_file("mainmenu.qvm", 1);
    patch_file("ingamemenu.qvm", 0);
}

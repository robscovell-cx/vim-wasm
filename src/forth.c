/*
 * forth.c — Full interactive Forth interpreter
 *
 * Integrated as a shell mode (no asyncify needed — line-by-line REPL).
 * Architecture: token-threaded with pointer-tagging for literals.
 *   body entries: low bit 0 → FWord pointer; low bit 1 → literal (value >> 1)
 * Control structures use backpatching into the compile buffer.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <setjmp.h>
#include <stdlib.h>

void terminal_write(const char *data, int len);
extern int forth_active;          /* defined in app.c */
void app_init(void);

/* ================================================================
 * Types and limits
 * ================================================================ */

typedef intptr_t  cell;
typedef uintptr_t ucell;
typedef uintptr_t body_t;

#define DSZ   256    /* data stack depth */
#define RSZ    64    /* return stack depth */
#define NW    1024   /* word pool entries */
#define BP   16384   /* body pool (body_t elements) */
#define SP    8192   /* string pool (bytes) */
#define CB     512   /* compile buffer */
#define CS      32   /* control-flow stack */
#define LSZ     16   /* loop stack depth */

#define F_IMM  0x80
#define F_HID  0x40

/* Pointer-tagged body entries */
#define BWORD(fw)   ((body_t)(fw))
#define BLIT(v)     ((body_t)((cell)(v) << 1 | 1))
#define IS_LIT(e)   ((e) & 1)
#define GET_LIT(e)  ((cell)((intptr_t)(e) >> 1))
#define BEND        ((body_t)0)

/* ================================================================
 * Word descriptor
 * ================================================================ */

typedef struct FWord {
    struct FWord *link;
    uint8_t       flags;
    char          name[32];
    void        (*fn)(void);   /* non-NULL: primitive */
    body_t       *body;        /* non-NULL: compiled body (BEND-terminated) */
    cell          value;       /* VARIABLE / CONSTANT / VALUE */
    const char   *str;         /* string literal */
    int           slen;
} FWord;

/* ================================================================
 * Global state
 * ================================================================ */

static FWord   wp[NW];        /* word pool */
static int     wn   = 0;
static FWord  *wlat = NULL;   /* latest word */

static body_t  bpool[BP];     /* body pool */
static int     btop = 0;

static char    spool[SP];     /* string pool */
static int     stop = 0;

static cell    ds[DSZ];       /* data stack */
static int     dsp = 0;

static cell    rs[RSZ];       /* return stack */
static int     rsp = 0;

static int     fstate = 0;    /* 0=interpret, 1=compile */
static int     fbase  = 10;

static body_t  cbuf[CB];      /* compile buffer */
static int     clen  = 0;
static FWord  *cword = NULL;

static int     ctrl[CS];      /* control-flow stack (cbuf positions) */
static int     ctop = 0;

static cell    li[LSZ], ll[LSZ];  /* loop index/limit stacks */
static int     ldp  = 0;         /* loop depth */

static body_t *g_ip   = NULL;    /* interpreter's instruction pointer */
static FWord  *g_word = NULL;    /* currently executing word */

static jmp_buf ferr;
static char    ferrmsg[128];

/* ================================================================
 * Error, output helpers
 * ================================================================ */

#define FERROR(msg) do { strncpy(ferrmsg, (msg), sizeof(ferrmsg)-1); \
                         longjmp(ferr, 1); } while(0)

static void fout(const char *s)           { terminal_write(s, (int)strlen(s)); }
static void foutc(char c)                 { terminal_write(&c, 1); }
static void fcr(void)                     { fout("\r\n"); }
static void foutf(const char *fmt, ...) {
    char buf[256]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    fout(buf);
}

/* ================================================================
 * Stack helpers
 * ================================================================ */

static void push(cell v) {
    if (dsp >= DSZ) FERROR("stack overflow");
    ds[dsp++] = v;
}
static cell pop(void) {
    if (dsp <= 0)  FERROR("stack underflow");
    return ds[--dsp];
}
static cell peek(int n) {        /* 0=TOS, 1=NOS … */
    if (dsp - 1 - n < 0) FERROR("stack underflow");
    return ds[dsp - 1 - n];
}
static void rpush(cell v) {
    if (rsp >= RSZ) FERROR("return stack overflow");
    rs[rsp++] = v;
}
static cell rpop(void) {
    if (rsp <= 0)  FERROR("return stack underflow");
    return rs[--rsp];
}

/* ================================================================
 * Dictionary
 * ================================================================ */

static FWord *find(const char *name, int len) {
    for (FWord *w = wlat; w; w = w->link) {
        if ((w->flags & F_HID) == 0 &&
            (int)strlen(w->name) == len &&
            strncasecmp(w->name, name, len) == 0)
            return w;
    }
    return NULL;
}

static FWord *defword(const char *name, void (*fn)(void)) {
    if (wn >= NW) FERROR("dictionary full");
    FWord *w = &wp[wn++];
    memset(w, 0, sizeof *w);
    w->link  = wlat;
    w->flags = 0;
    strncpy(w->name, name, 31);
    w->fn    = fn;
    wlat     = w;
    return w;
}

/* Allocate n body_t slots from the body pool */
static body_t *balloc(int n) {
    if (btop + n > BP) FERROR("body pool full");
    body_t *p = &bpool[btop];
    btop += n;
    return p;
}

/* Allocate a string in the string pool */
static const char *salloc(const char *s, int len) {
    if (stop + len + 1 > SP) FERROR("string pool full");
    char *p = &spool[stop];
    memcpy(p, s, len); p[len] = '\0';
    stop += len + 1;
    return p;
}

/* ================================================================
 * Inner interpreter
 * ================================================================ */

static void exec(FWord *w);   /* forward */

static void exec_body(body_t *body) {
    body_t *saved_ip = g_ip;
    g_ip = body;
    while (*g_ip != BEND) {
        body_t e = *g_ip++;
        if (IS_LIT(e)) {
            push(GET_LIT(e));
        } else {
            FWord *w = (FWord *)e;
            FWord *saved_word = g_word;
            g_word = w;
            if (w->fn) w->fn();
            else        exec_body(w->body);
            g_word = saved_word;
        }
    }
    g_ip = saved_ip;
}

static void exec(FWord *w) {
    FWord *saved = g_word;
    g_word = w;
    if (w->fn) w->fn();
    else        exec_body(w->body);
    g_word = saved;
}

/* ================================================================
 * Compile helpers
 * ================================================================ */

static void cemit(body_t e) {
    if (clen >= CB) FERROR("word too long");
    cbuf[clen++] = e;
}
static void cword_ref(FWord *w) { cemit(BWORD(w)); }
static void clit(cell v)        { cemit(BLIT(v)); }

/* commit the current compile buffer to a word */
static void commit_body(FWord *w) {
    body_t *b = balloc(clen + 1);
    memcpy(b, cbuf, clen * sizeof(body_t));
    b[clen] = BEND;
    w->body = b;
}

/* ================================================================
 * Branch primitives (modify g_ip — called from exec_body context)
 * ================================================================ */

static FWord w_branch, w_zbranch, w_loop, w_ploop;

static void prim_branch(void) {    /* unconditional forward/backward jump */
    cell off = GET_LIT(*g_ip++);
    g_ip += off;
}
static void prim_zbranch(void) {   /* jump if TOS == 0 */
    cell off = GET_LIT(*g_ip++);
    if (pop() == 0) g_ip += off;
}
static void prim_do_runtime(void) {
    if (ldp >= LSZ) FERROR("loop stack overflow");
    ll[ldp] = pop();  /* limit */
    li[ldp] = pop();  /* index */
    ldp++;
}
static void prim_loop_runtime(void) {
    cell off = GET_LIT(*g_ip++);
    li[ldp-1]++;
    if (li[ldp-1] < ll[ldp-1]) g_ip += off;
    else                        ldp--;
}
static void prim_ploop_runtime(void) {
    cell off = GET_LIT(*g_ip++);
    cell step = pop();
    li[ldp-1] += step;
    if ((step > 0 && li[ldp-1] < ll[ldp-1]) ||
        (step < 0 && li[ldp-1] > ll[ldp-1]))
        g_ip += off;
    else
        ldp--;
}

/* ================================================================
 * Stack primitives
 * ================================================================ */
static void prim_dup(void)   { push(peek(0)); }
static void prim_drop(void)  { pop(); }
static void prim_swap(void)  { cell a=pop(),b=pop(); push(a); push(b); }
static void prim_over(void)  { push(peek(1)); }
static void prim_rot(void)   { cell a=pop(),b=pop(),c=pop(); push(b); push(a); push(c); }
static void prim_nrot(void)  { cell a=pop(),b=pop(),c=pop(); push(a); push(c); push(b); }
static void prim_nip(void)   { cell a=pop(); pop(); push(a); }
static void prim_tuck(void)  { cell a=pop(),b=pop(); push(a); push(b); push(a); }
static void prim_2dup(void)  { push(peek(1)); push(peek(1)); }
static void prim_2drop(void) { pop(); pop(); }
static void prim_2swap(void) { cell a=pop(),b=pop(),c=pop(),d=pop();
                                push(b); push(a); push(d); push(c); }
static void prim_2over(void) { push(peek(3)); push(peek(3)); }
static void prim_qdp(void)   { if (peek(0) != 0) push(peek(0)); }
static void prim_depth(void) { push(dsp); }
static void prim_tor(void)   { rpush(pop()); }
static void prim_fromr(void) { push(rpop()); }
static void prim_rfetch(void){ push(rs[rsp-1]); }
static void prim_i(void)     { if (ldp<1) FERROR("not in loop"); push(li[ldp-1]); }
static void prim_j(void)     { if (ldp<2) FERROR("not in outer loop"); push(li[ldp-2]); }
static void prim_leave(void) { if (ldp>0) li[ldp-1]=ll[ldp-1]; } /* force exit next LOOP */

/* ================================================================
 * Arithmetic and comparison
 * ================================================================ */
static void prim_add(void)  { cell b=pop(),a=pop(); push(a+b); }
static void prim_sub(void)  { cell b=pop(),a=pop(); push(a-b); }
static void prim_mul(void)  { cell b=pop(),a=pop(); push(a*b); }
static void prim_div(void)  { cell b=pop(),a=pop(); if(!b)FERROR("div/0"); push(a/b); }
static void prim_mod(void)  { cell b=pop(),a=pop(); if(!b)FERROR("div/0"); push(a%b); }
static void prim_divmod(void){ cell b=pop(),a=pop(); if(!b)FERROR("div/0");
                                push(a%b); push(a/b); }
static void prim_abs(void)  { cell a=pop(); push(a<0?-a:a); }
static void prim_neg(void)  { push(-pop()); }
static void prim_max(void)  { cell b=pop(),a=pop(); push(a>b?a:b); }
static void prim_min(void)  { cell b=pop(),a=pop(); push(a<b?a:b); }
static void prim_1p(void)   { ds[dsp-1]++; }
static void prim_1m(void)   { ds[dsp-1]--; }
static void prim_2p(void)   { ds[dsp-1]+=2; }
static void prim_2m(void)   { ds[dsp-1]-=2; }
static void prim_2s(void)   { ds[dsp-1]*=2; }
static void prim_2d(void)   { ds[dsp-1]/=2; }
static void prim_ss(void)   { cell n=pop(),b=pop(),a=pop();
                               push((cell)((int64_t)a*b/n)); }   /* */
static void prim_eq(void)   { cell b=pop(),a=pop(); push(a==b?-1:0); }
static void prim_ne(void)   { cell b=pop(),a=pop(); push(a!=b?-1:0); }
static void prim_lt(void)   { cell b=pop(),a=pop(); push(a<b?-1:0);  }
static void prim_gt(void)   { cell b=pop(),a=pop(); push(a>b?-1:0);  }
static void prim_le(void)   { cell b=pop(),a=pop(); push(a<=b?-1:0); }
static void prim_ge(void)   { cell b=pop(),a=pop(); push(a>=b?-1:0); }
static void prim_0eq(void)  { push(pop()==0?-1:0); }
static void prim_0ne(void)  { push(pop()!=0?-1:0); }
static void prim_0lt(void)  { push(pop()<0?-1:0);  }
static void prim_0gt(void)  { push(pop()>0?-1:0);  }
static void prim_and(void)  { cell b=pop(),a=pop(); push(a&b); }
static void prim_or(void)   { cell b=pop(),a=pop(); push(a|b); }
static void prim_xor(void)  { cell b=pop(),a=pop(); push(a^b); }
static void prim_inv(void)  { ds[dsp-1]=~ds[dsp-1]; }
static void prim_lsh(void)  { cell n=pop(),a=pop(); push(a<<n); }
static void prim_rsh(void)  { cell n=pop(),a=pop(); push((cell)((ucell)a>>n)); }
static void prim_true(void) { push(-1); }
static void prim_false(void){ push(0);  }

/* ================================================================
 * Memory
 * ================================================================ */
static void prim_fetch(void)  { cell a=pop(); push(*(cell*)a); }
static void prim_store(void)  { cell a=pop(),v=pop(); *(cell*)a=v; }
static void prim_cfetch(void) { cell a=pop(); push(*(uint8_t*)a); }
static void prim_cstore(void) { cell a=pop(),v=pop(); *(uint8_t*)a=(uint8_t)v; }
static void prim_addstore(void){ cell a=pop(),v=pop(); *(cell*)a+=v; }
static void prim_variable(void){ push((cell)&g_word->value); }
static void prim_constant(void){ push(g_word->value); }
static void prim_value(void)   { push(g_word->value); }

/* ================================================================
 * I/O
 * ================================================================ */
static void prim_dot(void) {
    cell v = pop();
    char buf[64];
    if (fbase == 10) snprintf(buf, sizeof buf, "%ld ", (long)v);
    else if (fbase == 16) snprintf(buf, sizeof buf, "%lX ", (unsigned long)v);
    else if (fbase == 8)  snprintf(buf, sizeof buf, "%lo ", (unsigned long)v);
    else {
        /* generic base */
        char tmp[64]; int i = 0;
        ucell u = (ucell)v; bool neg = (fbase==10 && v < 0);
        if (neg) u = (ucell)-v;
        if (u == 0) tmp[i++] = '0';
        while (u) { int d=u%fbase; tmp[i++]=(d<10?'0'+d:'A'+d-10); u/=fbase; }
        if (neg) tmp[i++] = '-';
        buf[0] = 0; while (--i >= 0) { char c[2]={tmp[i],0}; strcat(buf,c); }
        strcat(buf, " ");
    }
    fout(buf);
}
static void prim_dots(void) {
    foutf("<%d> ", dsp);
    for (int i = 0; i < dsp; i++) {
        char buf[32];
        if (fbase == 16) snprintf(buf, sizeof buf, "%lX ", (unsigned long)ds[i]);
        else             snprintf(buf, sizeof buf, "%ld ", (long)ds[i]);
        fout(buf);
    }
}
static void prim_emit(void)   { char c=(char)pop(); foutc(c); }
static void prim_cr(void)     { fcr(); }
static void prim_space(void)  { fout(" "); }
static void prim_spaces(void) { int n=(int)pop(); while(n-->0) fout(" "); }
static void prim_type(void)   { int len=(int)pop(); char *a=(char*)pop();
                                  terminal_write(a, len); }
static void prim_dotstr(void) { fout(g_word->str); }   /* ." runtime */
static void prim_sstr(void)   { push((cell)g_word->str); push(g_word->slen); }  /* S" runtime */
static void prim_decimal(void){ fbase=10; }
static void prim_hex(void)    { fbase=16; }
static void prim_octal(void)  { fbase=8; }
static void prim_base_store(void){ fbase=(int)pop(); }
static void prim_base_fetch(void){ push(fbase); }
static void prim_page(void)   { fout("\x1B[2J\x1B[H"); }
static void prim_bye(void) {
    fout("Goodbye.\r\n");
    forth_active = 0;
    app_init();
    longjmp(ferr, 2);  /* exit without error message */
}

/* ================================================================
 * Compiler words (runtime helpers created inline)
 * ================================================================ */

/* LITERAL: at runtime push the next cell from body as-is (but we use BLIT encoding) */
/* Nothing needed — the encoder/decoder handles this. */

/* ================================================================
 * Tokenizer
 * ================================================================ */

static const char *tsrc = NULL;
static int         tsrc_len = 0;

static void skip_ws(void) {
    while (tsrc_len > 0 && (*tsrc == ' ' || *tsrc == '\t')) { tsrc++; tsrc_len--; }
}

/* Parse next token into buf, return length (0 = EOF) */
static int next_token(char *buf, int bufsz, char delim) {
    if (delim == ' ') skip_ws();
    int i = 0;
    while (tsrc_len > 0 && (delim == ' ' ? (*tsrc != ' ' && *tsrc != '\t')
                                          : *tsrc != delim)) {
        if (i < bufsz - 1) buf[i++] = *tsrc;
        tsrc++; tsrc_len--;
    }
    if (delim != ' ' && tsrc_len > 0) { tsrc++; tsrc_len--; } /* consume delimiter */
    buf[i] = '\0';
    return i;
}

/* Parse a number in current base. Return true on success. */
static bool parse_num(const char *s, int len, cell *out) {
    if (len == 0) return false;
    bool neg = false;
    int i = 0;
    if (s[0] == '-') { neg = true; i = 1; }
    if (i == len) return false;
    /* detect base prefix */
    int base = fbase;
    if (s[i] == '$') { base = 16; i++; }
    else if (s[i] == '#') { base = 10; i++; }
    else if (s[i] == '%') { base = 2;  i++; }
    else if (s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X') && len > i+2) {
        base = 16; i += 2;
    }
    cell v = 0;
    for (; i < len; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9')      d = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return false;
        if (d >= base) return false;
        v = v * base + d;
    }
    *out = neg ? -v : v;
    return true;
}

/* ================================================================
 * Compile-time control structure helpers
 * ================================================================ */

static void ctrl_push(int v) {
    if (ctop >= CS) FERROR("control stack overflow");
    ctrl[ctop++] = v;
}
static int ctrl_pop(void) {
    if (ctop <= 0) FERROR("control stack underflow");
    return ctrl[--ctop];
}

/* Emit a forward branch (zbranch or branch), record patch location */
static int emit_fwd_branch(FWord *bw) {
    cword_ref(bw);
    int loc = clen;
    clit(0);      /* placeholder offset */
    return loc;
}
/* Backpatch a forward branch */
static void backpatch(int loc) {
    int offset = clen - loc - 1;  /* jump over body after the offset */
    cbuf[loc] = BLIT(offset);
}
/* Emit an unconditional backward jump to a saved position */
static void emit_bwd_branch(int dest) {
    cword_ref(&w_branch);
    int offset = dest - clen - 1;   /* negative offset back to dest */
    clit(offset);
}

/* ================================================================
 * Interpreter words (IMMEDIATE — execute immediately in compile mode)
 * ================================================================ */

static void prim_colon(void) {
    /* : — start a new word definition */
    char name[32];
    next_token(name, sizeof name, ' ');
    if (!name[0]) FERROR(": needs a name");
    /* Create word (hidden until ; completes) */
    FWord *w = defword(name, NULL);
    w->flags |= F_HID;
    cword = w; clen = 0;
    fstate = 1;
}

static void prim_semicolon(void) {
    /* ; — finish definition */
    if (!cword) FERROR("; without :");
    commit_body(cword);
    cword->flags &= ~F_HID;
    cword = NULL; clen = 0;
    fstate = 0;
}

static void prim_immediate(void) {
    if (wlat) wlat->flags |= F_IMM;
}

/* IF ( flag -- ) */
static void prim_if(void) {
    ctrl_push(emit_fwd_branch(&w_zbranch));
}
/* ELSE */
static void prim_else(void) {
    int if_loc = ctrl_pop();
    int else_loc = emit_fwd_branch(&w_branch);
    backpatch(if_loc);
    ctrl_push(else_loc);
}
/* THEN */
static void prim_then(void)   { backpatch(ctrl_pop()); }
/* BEGIN */
static void prim_begin(void)  { ctrl_push(clen); }
/* UNTIL ( flag -- ) */
static void prim_until(void) {
    int dest = ctrl_pop();
    cword_ref(&w_zbranch);
    clit(dest - clen - 1);
}
/* AGAIN */
static void prim_again(void)  { emit_bwd_branch(ctrl_pop()); }
/* WHILE ( flag -- ) */
static void prim_while(void)  {
    int begin = ctrl_pop();
    int wh = emit_fwd_branch(&w_zbranch);
    ctrl_push(begin);
    ctrl_push(wh);
}
/* REPEAT */
static void prim_repeat(void) {
    int wh    = ctrl_pop();
    int begin = ctrl_pop();
    emit_bwd_branch(begin);
    backpatch(wh);
}
/* DO */
static FWord w_do_rt;
static void prim_do(void) {
    cword_ref(&w_do_rt);
    ctrl_push(clen);   /* start of loop body */
}
/* LOOP */
static void prim_loop_c(void) {
    int dest = ctrl_pop();
    cword_ref(&w_loop);
    clit(dest - clen - 1);
}
/* +LOOP */
static void prim_ploop_c(void) {
    int dest = ctrl_pop();
    cword_ref(&w_ploop);
    clit(dest - clen - 1);
}
/* RECURSE — compile a reference to the word currently being defined */
static void prim_recurse(void) {
    if (!cword) FERROR("RECURSE outside definition");
    cword_ref(cword);
}

/* LITERAL — take TOS and compile it as a literal in the body */
static void prim_literal(void) {
    clit(pop());
}

/* [ and ] — switch to/from interpret mode inside a definition */
static void prim_lbracket(void) { fstate = 0; }
static void prim_rbracket(void) { fstate = 1; }

/* POSTPONE — compile a call to a word */
static void prim_postpone(void) {
    char name[32]; int n = next_token(name, sizeof name, ' ');
    FWord *w = find(name, n);
    if (!w) FERROR("POSTPONE: unknown word");
    cword_ref(w);
}

/* VARIABLE */
static void prim_variable_def(void) {
    char name[32];
    int n = next_token(name, sizeof name, ' ');
    if (!n) FERROR("VARIABLE needs a name");
    FWord *w = defword(name, prim_variable);
    w->value = 0;
}
/* CONSTANT */
static void prim_constant_def(void) {
    char name[32];
    int n = next_token(name, sizeof name, ' ');
    if (!n) FERROR("CONSTANT needs a name");
    cell v = pop();
    FWord *w = defword(name, prim_constant);
    w->value = v;
}
/* VALUE */
static void prim_value_def(void) {
    char name[32];
    int n = next_token(name, sizeof name, ' ');
    if (!n) FERROR("VALUE needs a name");
    cell v = pop();
    FWord *w = defword(name, prim_value);
    w->value = v;
}
/* TO — update a VALUE */
static void prim_to(void) {
    char name[32];
    int n = next_token(name, sizeof name, ' ');
    FWord *w = find(name, n);
    if (!w || w->fn != prim_value) FERROR("TO: not a VALUE");
    if (fstate) {
        /* compile: push addr of value field, store */
        clit((cell)&w->value);
        FWord *sw = find("!", 1);
        if (sw) cword_ref(sw);
    } else {
        w->value = pop();
    }
}

/* ." — print string literal */
static void prim_dotquote(void) {
    char buf[256]; int n = next_token(buf, sizeof buf, '"');
    if (fstate) {
        /* compile: create anonymous word that prints the string */
        const char *s = salloc(buf, n);
        FWord *w = defword("\"", prim_dotstr);
        w->str = s; w->slen = n; w->flags |= F_HID;
        cword_ref(w);
    } else {
        terminal_write(buf, n);
    }
}

/* S" — string literal */
static void prim_squote(void) {
    char buf[256]; int n = next_token(buf, sizeof buf, '"');
    if (fstate) {
        const char *s = salloc(buf, n);
        FWord *w = defword("s\"", prim_sstr);
        w->str = s; w->slen = n; w->flags |= F_HID;
        cword_ref(w);
    } else {
        const char *s = salloc(buf, n);
        push((cell)s); push(n);
    }
}

/* CHAR — push ASCII code of next token's first char */
static void prim_char(void) {
    char buf[32]; next_token(buf, sizeof buf, ' ');
    push((cell)(unsigned char)buf[0]);
}

/* FORGET */
static void prim_forget(void) {
    char name[32]; int n = next_token(name, sizeof name, ' ');
    FWord *w = find(name, n);
    if (!w) FERROR("FORGET: unknown word");
    wlat = w->link;
    wn = (int)(w - wp);
}

/* WORDS */
static void prim_words(void) {
    int col = 0;
    for (FWord *w = wlat; w; w = w->link) {
        if (w->flags & F_HID) continue;
        int len = (int)strlen(w->name);
        if (col + len + 2 > 78) { fcr(); col = 0; }
        fout(w->name); fout("  ");
        col += len + 2;
    }
    fcr();
}

/* SEE — simple decompiler */
static void prim_see(void) {
    char name[32]; int n = next_token(name, sizeof name, ' ');
    FWord *w = find(name, n);
    if (!w) { fout("not found\r\n"); return; }
    foutf(": %s  ", w->name);
    if (w->fn) {
        fout("( primitive )\r\n");
        return;
    }
    for (body_t *b = w->body; *b != BEND; b++) {
        if (IS_LIT(*b)) foutf("#%d ", GET_LIT(*b));
        else             foutf("%s ", ((FWord*)*b)->name);
    }
    fout(";\r\n");
}

/* UNUSED */
static void prim_unused(void) {
    push((cell)(BP - btop) * (cell)sizeof(body_t));
}

/* DEPTH already done as prim_depth */
/* .BASE */
static void prim_dotbase(void) {
    foutf("base=%d ", fbase);
}

/* ================================================================
 * Outer interpreter — process one line
 * ================================================================ */

void forth_process_line(const char *line, int len) {
    tsrc = line; tsrc_len = len;

    int jval = setjmp(ferr);
    if (jval == 2) return;   /* clean BYE exit */
    if (jval == 1) {
        /* error recovery */
        fout("  ?  ");
        if (ferrmsg[0]) { fout(ferrmsg); ferrmsg[0] = 0; }
        fcr();
        fstate = 0; cword = NULL; clen = 0; ctop = 0; ldp = 0;
        dsp = 0; rsp = 0;
        return;
    }

    char tok[64];
    int tlen;
    while ((tlen = next_token(tok, sizeof tok, ' ')) > 0 || tsrc_len > 0) {
        if (!tlen) continue;

        FWord *w = find(tok, tlen);

        if (w) {
            if (fstate && !(w->flags & F_IMM)) {
                cword_ref(w);
            } else {
                exec(w);
            }
        } else {
            cell num;
            if (parse_num(tok, tlen, &num)) {
                if (fstate) clit(num);
                else        push(num);
            } else {
                foutf("%s ?  ", tok); fcr();
                fstate = 0; cword = NULL; clen = 0; ctop = 0;
                return;
            }
        }
    }

    if (!fstate) fout(" ok\r\n");
}

/* ================================================================
 * Initialization — register all built-in words
 * ================================================================ */

void forth_init(void) {
    wn = 0; wlat = NULL;
    btop = 0; stop = 0;
    dsp = 0; rsp = 0;
    fstate = 0; fbase = 10;
    clen = 0; cword = NULL; ctop = 0; ldp = 0;

    /* set up the special branch/loop words (not in word pool, used internally) */
    strcpy(w_branch.name,  "_BRANCH");  w_branch.fn  = prim_branch;
    strcpy(w_zbranch.name, "_0BRANCH"); w_zbranch.fn = prim_zbranch;
    strcpy(w_loop.name,    "_LOOP");    w_loop.fn    = prim_loop_runtime;
    strcpy(w_ploop.name,   "_+LOOP");   w_ploop.fn   = prim_ploop_runtime;
    strcpy(w_do_rt.name,   "_DO");      w_do_rt.fn   = prim_do_runtime;

    /* Stack */
    defword("DUP",   prim_dup);   defword("DROP",  prim_drop);
    defword("SWAP",  prim_swap);  defword("OVER",  prim_over);
    defword("ROT",   prim_rot);   defword("-ROT",  prim_nrot);
    defword("NIP",   prim_nip);   defword("TUCK",  prim_tuck);
    defword("2DUP",  prim_2dup);  defword("2DROP", prim_2drop);
    defword("2SWAP", prim_2swap); defword("2OVER", prim_2over);
    defword("?DUP",  prim_qdp);   defword("DEPTH", prim_depth);
    /* Return stack */
    defword(">R",  prim_tor);    defword("R>",  prim_fromr);
    defword("R@",  prim_rfetch);
    /* Loop indices */
    defword("I",   prim_i);      defword("J", prim_j);
    defword("LEAVE", prim_leave);
    /* Arithmetic */
    defword("+",   prim_add);    defword("-",   prim_sub);
    defword("*",   prim_mul);    defword("/",   prim_div);
    defword("MOD", prim_mod);    defword("/MOD",prim_divmod);
    defword("ABS", prim_abs);    defword("NEGATE",prim_neg);
    defword("MAX", prim_max);    defword("MIN", prim_min);
    defword("1+",  prim_1p);     defword("1-",  prim_1m);
    defword("2+",  prim_2p);     defword("2-",  prim_2m);
    defword("2*",  prim_2s);     defword("2/",  prim_2d);
    defword("*/",  prim_ss);
    /* Comparison */
    defword("=",   prim_eq);     defword("<>",  prim_ne);
    defword("<",   prim_lt);     defword(">",   prim_gt);
    defword("<=",  prim_le);     defword(">=",  prim_ge);
    defword("0=",  prim_0eq);    defword("0<>", prim_0ne);
    defword("0<",  prim_0lt);    defword("0>",  prim_0gt);
    /* Logic */
    defword("AND",    prim_and); defword("OR",   prim_or);
    defword("XOR",    prim_xor); defword("INVERT",prim_inv);
    defword("LSHIFT", prim_lsh); defword("RSHIFT",prim_rsh);
    defword("TRUE",   prim_true);defword("FALSE",prim_false);
    /* Memory */
    defword("@",   prim_fetch);  defword("!",   prim_store);
    defword("C@",  prim_cfetch); defword("C!",  prim_cstore);
    defword("+!",  prim_addstore);
    /* I/O */
    defword(".",   prim_dot);    defword(".S",  prim_dots);
    defword("EMIT",prim_emit);   defword("CR",  prim_cr);
    defword("SPACE",prim_space); defword("SPACES",prim_spaces);
    defword("TYPE",prim_type);
    defword("DECIMAL",prim_decimal); defword("HEX",prim_hex);
    defword("OCTAL",prim_octal);
    defword("BASE!", prim_base_store); defword("BASE@",prim_base_fetch);
    defword(".BASE",prim_dotbase);
    defword("PAGE", prim_page);
    defword("BYE",  prim_bye);
    /* Compiler */
    FWord *w;
    w = defword(":",   prim_colon);   /* : does not compile its argument */
    w = defword(";",   prim_semicolon); w->flags |= F_IMM;
    w = defword("IMMEDIATE", prim_immediate);
    w = defword("IF",    prim_if);    w->flags |= F_IMM;
    w = defword("ELSE",  prim_else);  w->flags |= F_IMM;
    w = defword("THEN",  prim_then);  w->flags |= F_IMM;
    w = defword("BEGIN", prim_begin); w->flags |= F_IMM;
    w = defword("UNTIL", prim_until); w->flags |= F_IMM;
    w = defword("AGAIN", prim_again); w->flags |= F_IMM;
    w = defword("WHILE", prim_while); w->flags |= F_IMM;
    w = defword("REPEAT",prim_repeat);w->flags |= F_IMM;
    w = defword("DO",    prim_do);    w->flags |= F_IMM;
    w = defword("LOOP",  prim_loop_c);w->flags |= F_IMM;
    w = defword("+LOOP", prim_ploop_c);w->flags |= F_IMM;
    w = defword("RECURSE",prim_recurse);w->flags |= F_IMM;
    w = defword("LITERAL",prim_literal);w->flags |= F_IMM;
    w = defword("[",  prim_lbracket); w->flags |= F_IMM;
    w = defword("]",  prim_rbracket);
    w = defword("POSTPONE",prim_postpone);w->flags |= F_IMM;
    w = defword(".\"", prim_dotquote);w->flags |= F_IMM;
    w = defword("S\"", prim_squote); w->flags |= F_IMM;
    w = defword("CHAR",prim_char);
    w = defword("VARIABLE",prim_variable_def);
    w = defword("CONSTANT",prim_constant_def);
    w = defword("VALUE",   prim_value_def);
    w = defword("TO",      prim_to);  w->flags |= F_IMM;
    w = defword("FORGET",  prim_forget);
    w = defword("WORDS",   prim_words);
    w = defword("SEE",     prim_see);
    w = defword("UNUSED",  prim_unused);
    (void)w;

    /* A few useful definitions in Forth itself */
    forth_process_line(": NOT  0= ;", 12);
    forth_process_line(": NAND  AND INVERT ;", 20);
    forth_process_line(": NOR   OR  INVERT ;", 20);
    forth_process_line(": ABS   DUP 0< IF NEGATE THEN ;", 31);
    forth_process_line(": WITHIN  OVER - >R - R> U< ;", 30);
    forth_process_line(": CELLS  4 * ;", 14);
    forth_process_line(": CELL+  4 + ;", 14);
    forth_process_line(": CHARS  1 * ;", 14);
    forth_process_line(": CHAR+  1 + ;", 14);
    forth_process_line(": NL  10 EMIT ;", 15);
    forth_process_line(": .H  BASE@ >R HEX . R> BASE! ;", 32);
    forth_process_line(": .B  BASE@ >R 2 BASE! . R> BASE! ;", 36);
    forth_process_line(": U.  0 <# #S #> TYPE SPACE ;", 29); /* simplified */
    /* Redefine ABS properly (remove the Forth one so C one takes precedence) */
    /* (the C one is already registered before the Forth one, so it shadows it) */
}

/* ================================================================
 * Public API — called from app.c
 * ================================================================ */

void forth_start(void) {
    forth_init();
    forth_active = 1;
    fout("\x1B[0;32m");
    fout("  Forth  --  type WORDS for dictionary, BYE to exit\r\n\r\n");
    fout("forth> ");
    fout("\x1B[0m");
}

/* ================================================================
 * forth_handle_key — called from app.c for each keydown event
 * Accumulates a line; on Enter calls forth_process_line.
 * Reuses the shell's line_buf/line_len from app.c.
 * ================================================================ */

/* We use app.c's existing line_buf, so we need to replicate its key handling. */
#define FORTH_LINE 512
static char forth_line[FORTH_LINE];
static int  forth_llen = 0;

void forth_handle_key(const char *key) {
    if (!forth_active) return;

    if (strcmp(key, "Enter") == 0) {
        forth_line[forth_llen] = '\0';
        fout("\r\n");
        forth_process_line(forth_line, forth_llen);
        forth_llen = 0;
        if (forth_active) {
            fout("\x1B[0;32mforth> \x1B[0m");
        }
        return;
    }

    if (strcmp(key, "Backspace") == 0) {
        if (forth_llen > 0) {
            forth_llen--;
            fout("\x08 \x08");
        }
        return;
    }

    if (strlen(key) != 1) return;
    char c = key[0];
    if (c >= 0x20 && c < 0x7F && forth_llen < FORTH_LINE - 1) {
        forth_line[forth_llen++] = c;
        char echo[2] = {c, '\0'};
        fout(echo);
    }
}

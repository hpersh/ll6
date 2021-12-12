#ifdef __cplusplus
extern "C" {
#else
typedef unsigned char bool;
#define true   1
#define false  0    
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#ifndef CYGWIN
#include <link.h>
#endif
#include <elf.h>
#include <assert.h>
#include <zlib.h>

#ifdef NDEBUG
#define DEBUG_ASSERT(x)
#else
#define DEBUG_ASSERT  assert
#endif

#define ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))
#define END(a)         (&(a)[ARRAY_SIZE(a)])

#define FIELD_OFS(s, f)  ((uintptr_t) &((s *) 0)->f)
#define FIELD_PTR_TO_STRUCT_PTR(p, s, f)  ((s *)((unsigned char *) p - FIELD_OFS(s, f)))
    
struct dllist {
    struct dllist *prev, *next;
};

#define DLLIST_INIT(nm)       { (nm), (nm) }
#define DLLIST_DECL_INIT(nm)  struct dllist nm[1] = { DLLIST_INIT(nm) }

static inline void dllist_init(struct dllist *li)
{
    li->prev = li->next = li;
}


static inline struct dllist *dllist_first(const struct dllist *li)
{
    return (li->next);
}


static inline struct dllist *dllist_last(const struct dllist *li)
{
    return (li->prev);
}


static inline struct dllist *dllist_end(struct dllist *li)
{
    return (li);
}

  
static inline bool dllist_is_empty(const struct dllist *li)
{
    DEBUG_ASSERT((li->prev == li) == (li->next == li));
    
    return (li->next == li);
}
  
struct dllist *dllist_insert(struct dllist *nd, struct dllist *before);
struct dllist *dllist_erase(struct dllist *nd);

typedef struct sx *sx_t;
typedef long long int intval_t;
typedef long double floatval_t;
typedef void (*codefuncval_t)(void);

void except_num_args(unsigned expected);
void except_num_args_min(unsigned min);
void except_num_args_range(unsigned min, unsigned max);

sx_t     *vm_dst(void);    
unsigned vm_argc(void);
sx_t     vm_args(void);
    
static inline void cf_argc_chk(unsigned n)
{
    if (vm_argc() != n)  except_num_args(n);
}


static inline void cf_argc_chk_min(unsigned n)
{
    if (vm_argc() < n)  except_num_args_min(n);
}


static inline void cf_argc_chk_range(unsigned a, unsigned b)
{
    unsigned argc = vm_argc();
    if (!(argc >= a && argc <= b))  except_num_args_range(a, b);
}


enum sx_type {
    SX_TYPE_NIL = 0,
    SX_TYPE_INT,
    SX_TYPE_FLOAT,
    SX_TYPE_COMPLEX,            /* N.B. Order for numeric types matters */
    SX_TYPE_STR,                /* N.B. Order of string types matters */
    SX_TYPE_SYM,
    SX_TYPE_NSUBR,
    SX_TYPE_SUBR,
    SX_TYPE_FILE,
    SX_TYPE_BARRAY,
    SX_TYPE_BLOB,
    SX_TYPE_ARRAY,
    SX_TYPE_EARRAY,
    SX_TYPE_SET,                /* N.B. Order of set types matters */
    SX_TYPE_DICT,
    SX_TYPE_CLOSURE,
    SX_TYPE_MODULE,
    SX_TYPE_DPTR,
    SX_TYPE_OBJ,
    SX_TYPE_CLASS,
    SX_TYPE_METHOD,
    SX_TYPE_LISTITER,
    SX_TYPE_BARRAYITER,
    SX_TYPE_ARRAYITER,
    SX_TYPE_SETITER,
    SX_TYPE_CLOSUREGEN,
    SX_TYPE_LAST
};

struct sx_complexval {
    floatval_t re, im;
};

struct sx_strval {      /* Used for both SX_TYPE_STR and SX_TYPE_SYM */
    unsigned   size;
    const char *data;
};

struct sx_codefuncval {     /* Used by both SX_TYPE_NSUBR and SX_TYPE_SUBR */
    codefuncval_t func;
    sx_t          name;
};

struct sx_fileval {
    sx_t filename, mode;
    FILE *fp;
};

struct sx_barrayval {
    unsigned      size;
    unsigned char *data;
};

struct blobhooks {
    void     (*mark)(sx_t);
    void     (*free)(sx_t);
    void     (*cleanup)(sx_t);
};

struct sx_blobval {
    sx_t                   inst_of;
    const struct blobhooks *hooks;
    unsigned               size;
    unsigned char          *data;
};

struct sx_arrayval {
    unsigned  size;             /* Array size, or used size for Earray */
    sx_t      *data;
};

struct sx_earrayval {
    struct sx_arrayval base[1]; /* size in base type is number of entries in use */
    unsigned           capacity;
};

struct sx_setval {      /* Used for both SX_TYPE_SET and SX_TYPE_DICT */
    struct sx_arrayval base[1];
    unsigned           cnt;
};

struct map_node {
    struct map_node_parent {
        struct map_node *node;
        int             cmp;
    } parent[1];
    sx_t key, value;
    struct map_node_child {
        struct map_node *node;
        unsigned        height;
    } child[2];
};

struct sx_mapval {
    struct map_node *root;
    unsigned        cnt;
};
    
struct sx_closureval {
    sx_t expr, dict;
};

struct sx_moduleval {
    sx_t name, sha1, dict;
    void *dlhdl;
};

struct sx_dptrval {
    sx_t car, cdr;
};

struct sx_objval {
    sx_t inst_of;
    sx_t dict;
};
    
struct sx_classval {
    sx_t inst_of;
    struct class {
        sx_t name, parent, class_vars, class_methods, inst_methods;
    } *class;
};

struct sx_methodval {
    sx_t class, func;
};

struct sx_iterval {
    sx_t     sx;
    unsigned idx;
    sx_t     li;
};

struct sx_closuregen {
    sx_t cl, dict;
};
    
#define OFS_CLASS_METHODS  FIELD_OFS(struct class, class_methods)
#define OFS_INST_METHODS   FIELD_OFS(struct class, inst_methods)

struct sx {
    struct dllist list_node[1];     /* Link for free or in-use list */
    unsigned ref_cnt;
    struct {
        unsigned mem_borrowed : 1;  /* True => memory is borrowed, not owned */
        unsigned permanent : 1;     /* True => keep symbol always */
        unsigned hash_valid : 1;    /* True => hash field is valid */
        unsigned frozen : 1;        /* True => immutable */
        unsigned visited : 1;       /* For detecting circular references */
    } flags;
    unsigned hash;
    enum sx_type type;
    union {
        intval_t              intval;
        floatval_t            floatval;
        struct sx_complexval  complexval[1];
        struct sx_strval      strval[1]; /* Used by: SX_TYPE_STR, SX_TYPE_SYM */
        struct sx_codefuncval codefuncval[1]; /* Used by: SX_TYPE_SUBR, SX_TYPE_NSUBR */
        struct sx_fileval     fileval[1];
        struct sx_barrayval   barrayval[1];
        struct sx_blobval     blobval[1];
        struct sx_arrayval    arrayval[1];
        struct sx_earrayval   earrayval[1];
        struct sx_setval      setval[1]; /* Used by: SX_TYPE_SET, SX_TYPE_DICT */
        struct sx_mapval      mapval[1];
        struct sx_closureval  closureval[1];
        struct sx_moduleval   moduleval[1];
        struct sx_dptrval     dptrval[1];
        struct sx_objval      objval[1];
        struct sx_classval    classval[1];
        struct sx_methodval   methodval[1];
        struct sx_iterval     iterval[1];
        struct sx_closuregen  closuregenval[1];
    } u;
};

#define NIL ((sx_t) 0)

static inline unsigned sx_type(const sx_t sx)
{
    return (sx == 0 ? SX_TYPE_NIL : sx->type);
}

sx_t sx_inst_of(sx_t sx);

#define STR_CONST(s)   sizeof(s), s
#define STR_CONST1(s)  (sizeof(s) - 1), s

struct sx_numeric {
    enum sx_type type;
    union {
        intval_t             intval;
        floatval_t           floatval;
        struct sx_complexval complexval[1];
    } u;
};

static inline void numeric_from_int(struct sx_numeric *n, intval_t val)
{
    n->u.intval = val;
    n->type = SX_TYPE_INT;
}
    
static inline void numeric_from_float(struct sx_numeric *n, floatval_t val)
{
    n->u.floatval = val;
    n->type = SX_TYPE_FLOAT;
}
    
static inline void numeric_from_complex(struct sx_numeric *n, floatval_t re, floatval_t im)
{
    n->u.complexval->re = re;
    n->u.complexval->im = im;
    n->type = SX_TYPE_COMPLEX;
}

extern bool numeric_from_sx(struct sx_numeric *n, sx_t sx);
extern void numeric_convert(struct sx_numeric *n, unsigned to_type);

static inline void numeric_promote(struct sx_numeric *n, unsigned to_type)
{
    if (to_type <= n->type)  return;
    numeric_convert(n, to_type);
}

static inline unsigned numeric_type_max(const struct sx_numeric *n1, const struct sx_numeric *n2)
{
    unsigned result = n1->type;
    if (n2->type > result)  result = n2->type;
    return (result);
}
    
static inline bool sx_is_numeric(const sx_t sx)
{
    unsigned t = sx_type(sx);
    return (t >= SX_TYPE_INT && t <= SX_TYPE_COMPLEX);
}

static inline bool sx_is_str(const sx_t sx)
{
    unsigned t = sx_type(sx);
    return (t >= SX_TYPE_STR && t <= SX_TYPE_SYM);
}
    
static inline bool sx_is_set(const sx_t sx)
{
    unsigned t = sx_type(sx);
    return (t >= SX_TYPE_SET && t <= SX_TYPE_DICT);
}
    
extern void sx_retain (sx_t sx); 
extern void sx_release (sx_t sx); 
    
static inline void sx_assign_norelease(sx_t *dst, sx_t src)
{
    sx_retain(*dst = src);
}


static inline sx_t sx_assign(sx_t *dst, sx_t src)
{
    sx_t old = *dst;
    sx_assign_norelease(dst, src);
    sx_release(old);

    return (src);
}


static inline void sx_assign_nil(sx_t *dst)
{
    sx_release(*dst);
    *dst = 0;
}


static inline void sx_move(sx_t *dst, sx_t *src)
{
    sx_release(*dst);
    *dst = *src;
    *src = 0;
}


static inline void sx_swap(sx_t *x, sx_t *y)
{
    sx_t temp = *x;
    *x = *y;
    *y = temp;
}

    
static inline void sx_rot(sx_t *x, sx_t *y, sx_t *z)
{
    sx_t temp = *x;
    *x = *y;
    *y = *z;
    *z = temp;
}

    
static inline sx_t car(const sx_t sx)
{
    return (sx->u.dptrval->car);
}


static inline sx_t cadr(const sx_t sx)
{
    return (sx->u.dptrval->cdr->u.dptrval->car);
}


static inline sx_t cdr(const sx_t sx)
{
    return (sx->u.dptrval->cdr);
}

sx_t sx_alloc(void);

extern unsigned int round_up_to_power_of_2 (unsigned int val); 
extern int list_len (sx_t li); 
extern bool sx_is_callable (const sx_t sx);
void sx_mark(sx_t sx);
extern void sx_release (sx_t);
extern void sx_free (sx_t sx);

void *mem_alloc(unsigned size);
void *mem_realloc(void *p, unsigned old_size, unsigned new_size);
void mem_free(unsigned size, void *p);

extern sx_t bool_new (sx_t *dst, bool val); 
extern sx_t int_new (sx_t *dst, intval_t val); 
extern sx_t float_new (sx_t *dst, floatval_t val);
extern sx_t complex_new (sx_t *dst, floatval_t re, floatval_t im);
extern sx_t numeric_new (sx_t *dst, struct sx_numeric *val);
extern sx_t str_newc (sx_t *dst, unsigned int size, const char *s); 
extern sx_t str_newc1 (sx_t *dst, const char *s); 
extern sx_t str_newm (sx_t *dst, unsigned int size, const char *s); 
extern sx_t str_newb (sx_t *dst, unsigned int size, const char *s); 
extern sx_t sym_newc (sx_t *dst, unsigned int size, const char *s); 
extern sx_t sym_newm (sx_t *dst, unsigned int size, const char *s); 
extern sx_t sym_newb (sx_t *dst, unsigned int size, const char *s); 
extern sx_t subr_new(sx_t *dst, codefuncval_t func, sx_t name);
extern sx_t nsubr_new(sx_t *dst, codefuncval_t func, sx_t name);
extern sx_t cons (sx_t *dst, sx_t car, sx_t cdr); 
extern sx_t file_new (sx_t *dst, sx_t filename, sx_t mode, FILE *fp); 
sx_t array_new(sx_t *dst, intval_t size);
sx_t barray_new(sx_t *dst, intval_t size);
sx_t barray_newc(sx_t *dst, intval_t size, unsigned char *data);
sx_t barray_newm(sx_t *dst, intval_t size, unsigned char *data);
sx_t barray_newb(sx_t *dst, intval_t size, unsigned char *data);
sx_t set_new_type(sx_t *dst, unsigned type, unsigned size);
sx_t blob_new(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size);
sx_t blob_newc(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size, unsigned char *data);
sx_t blob_newm(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size, unsigned char *data);
sx_t blob_newb(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size, unsigned char *data);

static inline sx_t set_new(sx_t *dst, unsigned size)
{
    return (set_new_type(dst, SX_TYPE_SET, size));
}

static inline sx_t dict_new(sx_t *dst, unsigned size)
{
    return (set_new_type(dst, SX_TYPE_DICT, size));
}

extern sx_t closure_new (sx_t *dst, sx_t expr, sx_t dict); 
extern sx_t *eval_alloc (unsigned int n); 
extern void eval_push (sx_t sx); 
extern void eval_pop (unsigned int n); 
extern void eval_unwind (sx_t *old);

unsigned str_hashc(unsigned size, const char *data);
unsigned str_hash(struct sx_strval *s);

struct str_joinv_item {
    unsigned   size;
    const char *data;
};

extern sx_t str_join(sx_t *dst,
                     unsigned size,
                     unsigned ldr_size,  const char *ldr,
                     unsigned sep_size,  const char *sep,
                     unsigned trlr_size, const char *trlr,
                     sx_t li
                     );

extern unsigned sx_hash(sx_t sx);
extern bool sx_equal (sx_t sx1, const sx_t sx2); 
extern bool set_at (const struct sx_setval *s, sx_t key, unsigned int hash); 
extern void set_put (struct sx_setval *s, sx_t key, unsigned int hash); 
extern void set_puts (struct sx_setval *s, sx_t key, unsigned int hash); 
extern void set_del (struct sx_setval *s, sx_t key, unsigned int hash); 
extern void set_dels (struct sx_setval *s, sx_t key, unsigned int hash); 
extern sx_t dict_at (const struct sx_setval *s, sx_t key, unsigned int hash); 
extern sx_t dict_ats (const struct sx_setval *s, sx_t key, unsigned int hash); 
extern void dict_atput (struct sx_setval *s, sx_t key, unsigned int hash, sx_t value); 
extern void dict_atsput (struct sx_setval *s, sx_t key, unsigned int hash, sx_t value); 
extern void dict_del (struct sx_setval *s, sx_t key, unsigned int hash); 
extern struct sx_strval *sx_repr(sx_t *, const sx_t);
extern void sx_print (FILE *, const sx_t);
extern void backtrace (void); 
extern void frame_except_longjmp (void); 
extern void frame_prog_longjmp (int code, sx_t sx); 
extern void except_raise1 (void); 
extern void except_inv_op (void); 
extern void except_lookup_non_symbol (sx_t sx); 
extern void except_symbol_not_bound (sx_t sym); 
extern sx_t env_find (sx_t sym); 
extern void except_bind_non_symbol (sx_t sx); 
extern void env_bind (sx_t sym, sx_t val); 
extern void env_bind_dict (sx_t dict, sx_t sym, sx_t val); 

void *ebuf_push(unsigned capacity);
void ebuf_append(void *cookie, unsigned n, const char *data);
void ebuf_appendc(void *cookie, const char c);
const char *ebuf_data(void *cookie);
unsigned ebuf_size(void *cookie);
sx_t str_neweb(sx_t *dst, void *coookie);

extern char getchar_except_eof (FILE *fp); 
extern char getchar_skip_space_except_eof (FILE *fp); 
void sx_readb(sx_t *dst, FILE *fp, bool limit_valid, unsigned limit);
extern void except_bad_form (sx_t sx); 
extern void except_num_args (unsigned int expected); 
extern void except_num_args_min (unsigned int min); 
extern void except_num_args_range (unsigned int min, unsigned int max); 
extern void except_bad_func_defn (sx_t sx); 
extern void sx_eval (sx_t *, const sx_t sx);
extern bool sx_list (sx_t *dst, const sx_t sx); 
extern void sx_print (FILE *fp, const sx_t sx); 
extern void except_bad_arg (sx_t sx); 
extern void except_file_not_open (sx_t sx); 
FILE *sx_file_fp(sx_t sx);
extern void except_no_label (sx_t label); 
extern void except_goto_no_prog (void); 
extern void except_return_no_prog (void); 

struct const_init {
    unsigned name_size;
    char     *name;
    unsigned type;
    union {
        intval_t      intval;
        floatval_t    floatval;
        struct sx_strval strval[1];
    } u;
};

void consts_init(unsigned n, const struct const_init *init);

struct func_init {
    unsigned      sym_size;
    char          *sym_data;
    sx_t          (*constructor)(sx_t *, codefuncval_t, sx_t);
    codefuncval_t func;
};

void funcs_init(unsigned modname_size, const char *modname,
                unsigned n, const struct func_init *init
                );

struct class_init {
    sx_t       *dst;
    unsigned   name_size;
    const char *name_data;
    sx_t       *parent;
    void       (*init)(sx_t cl);
};

void classes_init(unsigned modname_size, const char *modname, unsigned n, const struct class_init *init);

struct method_init {
    sx_t          *cl;
    unsigned      dict_ofs;
    unsigned      sel_size;
    const char    *sel;
    sx_t          (*constructor)(sx_t *, codefuncval_t, sx_t);
    codefuncval_t func;
};

void methods_init(unsigned modname_size, const char *modname,
                  unsigned n, const struct method_init *init
                  );
void methods_destroy(unsigned n, const struct method_init *init);

struct {
    sx_t module_main;
    sx_t nil, t, quote, lambda, nlambda, macro, function;
    sx_t eval, hash, repr, tostring, tolist, equal, cmp, call, call1, calln, __loaded__, __init__;
    sx_t copy, copydeep, ate, atput, setq;
    sx_t default_size, path, current;
    sx_t togenerator, prev, next, reset;

    sx_t Metaclass, Obj;
    sx_t Int, Float, Complex;
    sx_t String, Symbol;
    sx_t Nsubr, Subr;
    sx_t File;
    sx_t Barray;
    sx_t Array, Earray, Set, Dict;
    sx_t Closure, Env, Module;
    sx_t Dptr, List;
    sx_t Method;
    sx_t Generator, ListIter, BarrayIter, ArrayIter, SetIter, ClosureGenerator;
} main_consts;

sx_t *vm_dst(void);
unsigned vm_argc(void);
sx_t vm_args(void);

#ifdef cplusplus
}
#endif


#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <unistd.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <math.h>
#include <errno.h>

#include "ll.h"

static bool initf = true;
static bool echof;

/***************************************************************************
 *
 * Miscellaneous
 *
 ***************************************************************************/

struct dllist *dllist_insert(struct dllist *nd, struct dllist *before)
{
    struct dllist *p = before->prev;

    nd->prev = p;
    nd->next = before;

    return (p->next = before->prev = nd);
}


struct dllist *dllist_erase(struct dllist *nd)
{
    struct dllist *p = nd->prev, *q = nd->next;

    p->next = q;
    q->prev = p;
#ifndef NDEBUG
    nd->prev = nd->next = 0;
#endif

    return (nd);
}


unsigned round_up_to_power_of_2(unsigned val)
{
    unsigned result = val;
    for (;;) {
        unsigned k = val & (val - 1);
        if (k == 0)  return (result);
        result = (val = k) << 1;
        DEBUG_ASSERT(result != 0);
    }
}


bool slice(intval_t *pofs, intval_t *plen, intval_t size)
{
    intval_t ofs = *pofs, len = *plen;
    if (ofs < 0)  ofs += size;
    if (len < 0)  {
        ofs += len;
        len = -len;
    }
    if (ofs < 0 || (ofs + len) > size)  return (false);
    *pofs = ofs;
    *plen = len;
    
    return (true);
}


bool slice1(intval_t *pofs, intval_t size)
{
    intval_t len = 1;
    return (slice(pofs, &len, size));
}


int list_len(sx_t li)
{
    int result;
    for (result = 0; li != 0; li = cdr(li), ++result) {
        if (li->type != SX_TYPE_DPTR)  return (-(result + 1));
    }

    return (result);
}


bool list_len_chk(sx_t li, int n)
{
    for (; li != 0 && n > 0; li = cdr(li), --n) {
        if (li->type != SX_TYPE_DPTR)  return (false);
    }

    return (li == 0 && n == 0);
}


bool list_len_chk_min(sx_t li, int n)
{
    for (; li != 0 && n > 0; li = cdr(li), --n) {
        if (li->type != SX_TYPE_DPTR)  return (false);
    }

    return (n == 0);
}


sx_t sx_inst_of(sx_t sx)
{
    switch (sx_type(sx)) {
    case SX_TYPE_NIL:
        return (main_consts.Obj);
    case SX_TYPE_INT:
        return (main_consts.Int);
    case SX_TYPE_FLOAT:
        return (main_consts.Float);
    case SX_TYPE_COMPLEX:
        return (main_consts.Complex);
    case SX_TYPE_STR:
        return (main_consts.String);
    case SX_TYPE_SYM:
        return (main_consts.Symbol);
    case SX_TYPE_NSUBR:
        return (main_consts.Nsubr);
    case SX_TYPE_SUBR:
        return (main_consts.Subr);
    case SX_TYPE_FILE:
        return (main_consts.File);
    case SX_TYPE_BARRAY:
        return (main_consts.Barray);
    case SX_TYPE_ARRAY:
        return (main_consts.Array);
    case SX_TYPE_EARRAY:
        return (main_consts.Earray);
    case SX_TYPE_SET:
        return (main_consts.Set);
    case SX_TYPE_DICT:
        return (main_consts.Dict);
    case SX_TYPE_CLOSURE:
        return (main_consts.Closure);
    case SX_TYPE_DPTR:
        return (main_consts.Dptr);
    case SX_TYPE_MODULE:
        return (main_consts.Module);
    case SX_TYPE_METHOD:
        return (main_consts.Method);
    case SX_TYPE_LISTITER:
        return (main_consts.ListIter);
    case SX_TYPE_BARRAYITER:
        return (main_consts.BarrayIter);
    case SX_TYPE_ARRAYITER:
        return (main_consts.ArrayIter);
    case SX_TYPE_SETITER:
        return (main_consts.SetIter);
    case SX_TYPE_CLOSUREGEN:
        return (main_consts.ClosureGenerator);
    case SX_TYPE_CLASS:
        return (sx->u.classval->inst_of);
    case SX_TYPE_BLOB:
        return (sx->u.blobval->inst_of);
    case SX_TYPE_OBJ:
        return (sx->u.objval->inst_of);
    default: ;
    }

    assert(0);

    return (0);
}

/***************************************************************************
 *
 * Memory management
 *
 ***************************************************************************/

void sx_release(sx_t sx);

#ifndef NDEBUG

static struct {
    unsigned long long sx_pages_alloced;
    unsigned long long sx_pages_freed;
    unsigned long long sx_pages_in_use;
    unsigned long long sx_pages_in_use_max;
    unsigned long long sx_alloced;
    unsigned long long sx_freed;
    unsigned long long sx_collected;
    unsigned long long sx_in_use;
    unsigned long long sx_in_use_max;
    unsigned long long bytes_alloced;
    unsigned long long bytes_freed;
    unsigned long long bytes_collected;
    unsigned long long bytes_in_use;
    unsigned long long bytes_in_use_max;
} mem_stats;

#endif

static DLLIST_DECL_INIT(sx_free_list);
static struct dllist _sx_in_use_list[2] = { DLLIST_INIT(&_sx_in_use_list[0]), DLLIST_INIT(&_sx_in_use_list[1]) };
static struct dllist *sx_in_use_list_white = &_sx_in_use_list[0];
static struct dllist *sx_in_use_list_grey = &_sx_in_use_list[1];

static inline void sx_in_use_list_swap(void)
{
    struct dllist *temp = sx_in_use_list_white;
    sx_in_use_list_white = sx_in_use_list_grey;
    sx_in_use_list_grey = temp;
}

struct sx_page_hdr {
    unsigned in_use_cnt;
} __attribute__ ((aligned(128)));

struct sx_buf {
    struct sx_page_hdr *sx_page;
    struct sx sx[1];
} __attribute__ ((aligned(128)));

static unsigned mem_sx_page_size = 512 * 1024; /* Default is 512 kB */
#define SX_PER_PAGE    ((mem_sx_page_size - sizeof(struct sx_page_hdr)) / sizeof(struct sx_buf))

static bool collectingf;        /* Collect in progress -- see collect() */

static inline void *mem_pages_alloc(size_t size)
{
    return (mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0));
}


static inline void mem_pages_free(void *addr, size_t size)
{
    munmap(addr, size);
}


static void sx_free1(sx_t sx)
{
    /* Remove from in-use list, add to free list */
    dllist_erase(sx->list_node);
    dllist_insert(sx->list_node, dllist_end(sx_free_list));

    /* Check if page can be freed */
    struct sx_buf *sx_buf = FIELD_PTR_TO_STRUCT_PTR(sx, struct sx_buf, sx);
    struct sx_page_hdr *page = sx_buf->sx_page;
    if (--page->in_use_cnt == 0) {
        /* Page can be freed */
        
        struct sx_buf *p;
        unsigned n;
        for (p = (struct sx_buf *)(page + 1), n = SX_PER_PAGE; n > 0; --n, ++p) {
            dllist_erase(p->sx->list_node);
        }

        mem_pages_free(page, mem_sx_page_size);

#ifndef NDEBUG
        ++mem_stats.sx_pages_freed;
        assert(mem_stats.sx_pages_in_use > 0);
        --mem_stats.sx_pages_in_use;
#endif
    }

#ifndef NDEBUG
    ++mem_stats.sx_freed;
    assert(mem_stats.sx_in_use > 0);
    --mem_stats.sx_in_use;

    if (collectingf)  ++mem_stats.sx_collected;
#endif
}


void mem_free(unsigned size, void *p)
{
    free(p);

#ifndef NDEBUG
    mem_stats.bytes_freed += size;
    assert(mem_stats.bytes_in_use >= size);
    mem_stats.bytes_in_use -= size;

    if (collectingf)  mem_stats.bytes_collected += size;
#endif
}


static unsigned sx_mark_ref_cnt_incr(sx_t sx)
{
    ++sx->ref_cnt;
    assert(sx->ref_cnt != 0);
    if (sx->ref_cnt == 1) {
        dllist_erase(sx->list_node);
        dllist_insert(sx->list_node, dllist_end(sx_in_use_list_white));
    }

    return (sx->ref_cnt);
}


void sx_mark(sx_t sx)
{
    if (sx == 0 || sx_mark_ref_cnt_incr(sx) > 1)  return;

    switch (sx->type) {
    case SX_TYPE_SUBR:
    case SX_TYPE_NSUBR:
        sx_mark(sx->u.codefuncval->name);
        break;

    case SX_TYPE_FILE:
        {
            struct sx_fileval *f = sx->u.fileval;
            sx_mark(f->filename);
            sx_mark(f->mode);
        }
        break;

    case SX_TYPE_BLOB:
        {
            const struct blobhooks *h = sx->u.blobval->hooks;
            if (h == 0)  break;
            void (*f)(sx_t) = h->mark;
            if (f == 0)  break;
            (*f)(sx);
        }
        break;

    case SX_TYPE_ARRAY:
    case SX_TYPE_EARRAY:
    case SX_TYPE_SET:
    case SX_TYPE_DICT:   /* N.B. Assumes derived structures */
        {
            struct sx_arrayval *a = sx->u.arrayval;
            sx_t     *p;
            unsigned n;
            for (p = a->data, n = a->size; n > 0; --n, ++p) {
                sx_mark(*p);
            }
        }
        break;
        
    case SX_TYPE_CLOSURE:
        {
            struct sx_closureval *cl = sx->u.closureval;
            sx_mark(cl->expr);
            sx_mark(cl->dict);
        }
        break;

    case SX_TYPE_MODULE:
        sx_mark(sx->u.moduleval->dict);
        break;

    case SX_TYPE_DPTR:
        for (;;) {
            sx_mark(car(sx));
            sx_t next = cdr(sx);
            if (next == 0)  break;
            if (next->type != SX_TYPE_DPTR) {
                sx_mark(next);

                break;
            }
            sx = next;
            if (sx_mark_ref_cnt_incr(sx) > 1)  break;
        }

        return;

    case SX_TYPE_LISTITER:
    case SX_TYPE_BARRAYITER:
    case SX_TYPE_ARRAYITER:
    case SX_TYPE_SETITER:
        sx_mark(sx->u.iterval->sx);
        break;

    case SX_TYPE_CLOSUREGEN:
        {
            struct sx_closuregen *g = sx->u.closuregenval;
            sx_mark(g->cl);
            sx_mark(g->dict);
        }
        break;
        
    default:
        ;
    }
}

/* An sexpr is no longer in use when its reference count
   is 0.
   This can be either:
   - if an sexpr is released, or
   - if an sexpr was never marked during garbage collection.

   A release decrementing the reference count to 0 will invoke a
   free operation.  A reference count of 0 after marking will
   invoke a cleanup operation.

   The difference between free and cleanup is:
   - free MUST release references to other sexprs,
   - cleanup MUST NOT release references to other sexprs
     * Two cases:
       + cleanup is called after free
         In this case, references to other sexprs have already
         been released.
       + cleanup is called during garbage collection
         In this case, the object was unreachable during marking,
         so the reference counts for everything that was reachable
         during marking is already correct.
*/

void sx_cleanup(sx_t sx)
{
    /* In general:
       (1) release any OS resources allocated for this sexpr,
       (2) free any private memory allocated for this sexpr, and
       (3) free memory for this sexpr
     */

    switch (sx->type) {
    case SX_TYPE_STR:
    case SX_TYPE_SYM:
        {
            if (sx->flags.mem_borrowed)  break;
            
            /* Free private memory */
            
            struct sx_strval *s = sx->u.strval;
            mem_free(s->size, (void *) s->data);
        }
        break;
        
    case SX_TYPE_FILE:
        {
            /* Free OS resources */
            
            FILE *fp = sx->u.fileval->fp;
            if (fp != 0)  fclose(fp);
        }
        break;

    case SX_TYPE_BLOB:
        {
            /* Call cleanup hook */

            struct sx_blobval *b = sx->u.blobval;
            do {
                const struct blobhooks *h = b->hooks;
                if (h == 0)  break;
                void (*f)(sx_t) = h->cleanup;
                if (f != 0)  break;

                (*f)(sx);
            } while (0);

            if (sx->flags.mem_borrowed)  break;
            
            /* Free private memory */
            
            mem_free(b->size, b->data);
        }
        break;

    case SX_TYPE_BARRAY:
        {
            if (sx->flags.mem_borrowed)  break;
            
            /* Free private memory */
            
            struct sx_barrayval *b = sx->u.barrayval;
            mem_free(b->size, b->data);
        }
        break;

    case SX_TYPE_ARRAY:
    case SX_TYPE_SET:
    case SX_TYPE_DICT:   /* N.B. Assumes derived structures */
        {
            if (sx->flags.mem_borrowed)  break;
            
            /* Free private memory */
            
            struct sx_arrayval *a = sx->u.arrayval;
            mem_free(a->size * sizeof(a->data[0]), a->data);
        }
        break;
        
    case SX_TYPE_EARRAY:
        {
            if (sx->flags.mem_borrowed)  break;
            
            /* Free private memory */
            
            struct sx_earrayval *e = sx->u.earrayval;
            mem_free(e->capacity * sizeof(e->base->data[0]), e->base->data);
        }
        break;
        
    case SX_TYPE_MODULE:
        {
            struct sx_moduleval *m = sx->u.moduleval;
            void *dlhdl = m->dlhdl;
            if (dlhdl == 0)  break;
            
            /* Call module's destructor */

            struct sx_strval *n = m->name->u.strval;
            static const char ldr[] = "__";
            static const char trlr[] = "_destroy__";
            unsigned bufsize = sizeof(ldr) - 1
                + n->size - 1
                + sizeof(trlr) - 1
                + 1;
            char buf[bufsize];
            snprintf(buf, bufsize, "%s%s%s", ldr, n->data, trlr);
            void *addr = dlsym(dlhdl, buf);
            if (addr != 0)  (* (void (*)(void)) addr)();

            /* Release other sexprs -- deferred from sx_free() */

            if (!collectingf) {
                sx_release(m->name);
                sx_release(m->sha1);
                sx_release(m->dict);
            }

            /* Free OS resources */
            
            dlclose(dlhdl);
        }
        break;

    case SX_TYPE_CLASS:
        mem_free(sizeof(*sx->u.classval->class), sx->u.classval->class);
        break;

    default:
        ;
    }

    /* Free memory for given sx */
    
    sx_free1(sx);
}


void sx_free(sx_t sx)
{
    /* In general:
       (1) release any references to other sexprs, and
       (2) call cleanup
     */

    switch (sx->type) {
    case SX_TYPE_SUBR:
    case SX_TYPE_NSUBR:
        sx_release(sx->u.codefuncval->name);
        break;

    case SX_TYPE_FILE:
        {
            struct sx_fileval *f = sx->u.fileval;
            sx_release(f->filename);
            sx_release(f->mode);
        }
        break;

    case SX_TYPE_BLOB:
        {
            /* Call free hook */

            struct sx_blobval *b = sx->u.blobval;
            do {
                const struct blobhooks *h = b->hooks;
                if (h == 0)  break;
                void (*f)(sx_t) = h->free;
                if (f == 0)  break;
                
                (*f)(sx);
            } while (0);
            
            sx_release(b->inst_of);
        }
        break;

    case SX_TYPE_ARRAY:
    case SX_TYPE_EARRAY:
    case SX_TYPE_SET:
    case SX_TYPE_DICT:    /* N.B. Assumes derived structures */
        {
            struct sx_arrayval *a = sx->u.arrayval;
            sx_t     *p;
            unsigned n;
            for (p = a->data, n = a->size; n > 0; --n, ++p) {
                sx_release(*p);
            }
        }
        break;

    case SX_TYPE_CLOSURE:
        {
            struct sx_closureval *cl = sx->u.closureval;
            sx_release(cl->expr);
            sx_release(cl->dict);
        }
        break;

    case SX_TYPE_MODULE:
        /* Everything is deferred to cleanup time */
        break;

    case SX_TYPE_DPTR:
        /* To handle lists efficiently, they are a special case */
        
        for(;;) {
            sx_release(car(sx));
            sx_t next = cdr(sx);
            sx_free1(sx);
            if (next == 0)  break;
            if (next->type != SX_TYPE_DPTR) {
                sx_release(next);

                break;
            }
            sx = next;
            if (--sx->ref_cnt > 0)  break;
        }

        return;
        
    case SX_TYPE_CLASS:
        {
            struct sx_classval *cl = sx->u.classval;
            struct class *_cl = cl->class;
            sx_release(_cl->name);
            sx_release(_cl->parent);
            sx_release(_cl->class_vars);
            sx_release(_cl->class_methods);
            sx_release(_cl->inst_methods);
            sx_release(cl->inst_of);
        }
        break;

    case SX_TYPE_OBJ:
        {
            struct sx_objval *obj = sx->u.objval;
            sx_release(obj->dict);
            sx_release(obj->inst_of);
        }
        break;

    case SX_TYPE_LISTITER:
    case SX_TYPE_BARRAYITER:
    case SX_TYPE_ARRAYITER:
    case SX_TYPE_SETITER:
        sx_release(sx->u.iterval->sx);
        break;

    case SX_TYPE_CLOSUREGEN:
        {
            struct sx_closuregen *g = sx->u.closuregenval;
            sx_release(g->cl);
            sx_release(g->dict);
        }
        break;
        
    default:
        ;
    }

    sx_cleanup(sx);
}


void sx_retain(sx_t sx)
{
    if (sx == 0)  return;
    ++sx->ref_cnt;
    assert(sx->ref_cnt != 0);
}

static void sym_free(sx_t sx);
static void module_free(sx_t sx);

void sx_release(sx_t sx)
{
    if (sx == NIL)  return;
    assert(sx->ref_cnt != 0);
    --sx->ref_cnt;
    if (sx->ref_cnt == 0) {
        sx_free(sx);

        return;
    }
    switch (sx->type) {
    case SX_TYPE_SYM:
        /* Symbols are a special case, they always have circular references
           since they are held in the global symbol set
        */
        
        if (sx->ref_cnt == 1 && !sx->flags.permanent)  sym_free(sx);
        break;

    case SX_TYPE_MODULE:
        /* Modules are a special case, they always have circular references
           since they are held in the loaded dict
        */

        if (sx->ref_cnt == 1)  module_free(sx);
        break;
        
    default: ;
    }
}

static void collect(void), fatal(const char *mesg);     /* See VM */

static const char out_of_memory_mesg[] = "Out of memory";

sx_t sx_alloc(void)
{
    if (dllist_is_empty(sx_free_list)) {
        struct sx_page_hdr *page = 0;
        unsigned try_cnt = 0;
        for (;;) {
            ++try_cnt;
            page = (struct sx_page_hdr *) mem_pages_alloc(mem_sx_page_size);
            if ((void *) page != MAP_FAILED)  break;
            if (try_cnt > 1)  fatal(out_of_memory_mesg);
            
            collect();
        }
        struct sx_buf *p;
        unsigned n;
        for (p = (struct sx_buf *)(page + 1), n = SX_PER_PAGE; n > 0; --n, ++p) {
            p->sx_page = page;
            dllist_insert(p->sx->list_node, dllist_end(sx_free_list));
        }

#ifndef NDEBUG
        ++mem_stats.sx_pages_alloced;
        if (++mem_stats.sx_pages_in_use > mem_stats.sx_pages_in_use_max) {
            mem_stats.sx_pages_in_use_max = mem_stats.sx_pages_in_use;
        }
#endif
    }

    struct dllist *q = dllist_last(sx_free_list);
    dllist_erase(q);
    struct sx_buf *sx_buf = FIELD_PTR_TO_STRUCT_PTR(q, struct sx_buf, sx->list_node);
    sx_t result = sx_buf->sx;
    /* Need this, because free link is union and recycled memory is not cleared */
    result->ref_cnt = 0;
    result->flags.mem_borrowed
        = result->flags.permanent
        = result->flags.hash_valid
        = result->flags.frozen
        = result->flags.visited
        = false;
    
    ++sx_buf->sx_page->in_use_cnt;

    dllist_insert(result->list_node, dllist_end(sx_in_use_list_white));
    
#ifndef NDEBUG
    ++mem_stats.sx_alloced;
    if (++mem_stats.sx_in_use > mem_stats.sx_in_use_max) {
        mem_stats.sx_in_use_max = mem_stats.sx_in_use;
    }
#endif
    
    return (result);
}


void *mem_alloc(unsigned size)
{
    void *result = malloc(size);

#ifndef NDEBUG
    mem_stats.bytes_alloced += size;
    if ((mem_stats.bytes_in_use += size) > mem_stats.bytes_in_use_max) {
        mem_stats.bytes_in_use_max = mem_stats.bytes_in_use;
    }
#endif

    if (result == 0)  fatal(out_of_memory_mesg);
    
    return (result);
}


void *mem_realloc(void *p, unsigned old_size, unsigned new_size)
{
    void *result = realloc(p, new_size);

#ifndef NDEBUG
    if (new_size > old_size) {
        unsigned size = new_size - old_size;
        mem_stats.bytes_alloced += size;
        if ((mem_stats.bytes_in_use += size) > mem_stats.bytes_in_use_max) {
            mem_stats.bytes_in_use_max = mem_stats.bytes_in_use;
        }
    } else if (new_size < old_size) {
        unsigned size = old_size - new_size;
        mem_stats.bytes_freed += size;
        assert(mem_stats.bytes_in_use >= size);
        mem_stats.bytes_in_use -= size;
    }
#endif

    if (result == 0)  fatal(out_of_memory_mesg);

    return (result);
}

/***************************************************************************
 *
 * VM
 *
 ***************************************************************************/

#ifndef NDEBUG

struct {
    unsigned long long max_eval_stack_depth;
    unsigned long long max_frame_stack_depth;
} stack_stats;

#endif

void myexit(int code)
{
#ifndef NDEBUG
    fputs("Memory stats:\n", stderr);
    fprintf(stderr, "Sexpr pages alloced:     %llu\n", mem_stats.sx_pages_alloced);
    fprintf(stderr, "Sexpr pages freed:       %llu\n", mem_stats.sx_pages_freed);
    fprintf(stderr, "Sexpr pages in use:      %llu\n", mem_stats.sx_pages_in_use);
    fprintf(stderr, "Sexpr pages in use max:  %llu\n", mem_stats.sx_pages_in_use_max);
    fprintf(stderr, "Sexprs alloced:          %llu\n", mem_stats.sx_alloced);
    fprintf(stderr, "Sexprs freed:            %llu\n", mem_stats.sx_freed);
    fprintf(stderr, "Sexprs collected:        %llu\n", mem_stats.sx_collected);
    fprintf(stderr, "Sexprs in use:           %llu\n", mem_stats.sx_in_use);
    fprintf(stderr, "Sexprs in use max:       %llu\n", mem_stats.sx_in_use_max);
    fprintf(stderr, "Bytes alloced:           %llu\n", mem_stats.bytes_alloced);
    fprintf(stderr, "Bytes freed:             %llu\n", mem_stats.bytes_freed);
    fprintf(stderr, "Bytes collected:         %llu\n", mem_stats.bytes_collected);
    fprintf(stderr, "Bytes in use:            %llu\n", mem_stats.bytes_in_use);
    fprintf(stderr, "Bytes in use max:        %llu\n", mem_stats.bytes_in_use_max);
    fputs("Stack stats:\n", stderr);
    fprintf(stderr, "Max eval stack depth:  %llu\n", stack_stats.max_eval_stack_depth);
    fprintf(stderr, "Max frame stack depth: %llu\n", stack_stats.max_frame_stack_depth);
#endif
    
    exit(code);
}


void fatal(const char *mesg)
{
    fprintf(stderr, "%s\n", mesg);
    myexit(1);
}

struct frame {
    enum {
          FRAME_TYPE_FUNC,
          FRAME_TYPE_ENV,
          FRAME_TYPE_CLASS,
          FRAME_TYPE_MEM,
          FRAME_TYPE_EXCEPT,
          FRAME_TYPE_REP,
          FRAME_TYPE_PROG,
          FRAME_TYPE_WHILE,
          FRAME_TYPE_INPUT,
          NUM_FRAME_TYPES
    } type;
    struct frame *prev, *type_prev;
};

struct frame_func {
    struct frame base[1];
    sx_t *dst, func, args;
    int argc;
};

struct frame_env {
    struct frame base[1];
    struct frame_env *up;
    sx_t dict, module;
};

struct frame_mem {
    struct frame base[1];
    unsigned     size;
    void         *data;
};

struct ebuf {
    struct frame_mem base[1];
    unsigned ofs;
};

struct frame_class {
    struct frame base[1];
    sx_t class;
};

struct vm_state_save {
    sx_t          *sp;
    struct frame  *fp;

#ifndef NDEBUG
    unsigned debug_lvl;
#endif
};

struct frame_longjmp {
    struct frame base[1];
    volatile enum {
        FRAME_LONGJMP_JC_NONE,
        FRAME_LONGJMP_JC_EXCEPT_RAISE,
        FRAME_LONGJMP_JC_REP_RESTART,
        FRAME_LONGJMP_JC_REP_EXIT,
        FRAME_LONGJMP_JC_PROG_GOTO,
        FRAME_LONGJMP_JC_PROG_RETURN,
        FRAME_LONGJMP_JC_WHILE_CONTINUE,
        FRAME_LONGJMP_JC_WHILE_BREAK
    } jc;
    jmp_buf              jb;
    sx_t                 *arg;
    struct vm_state_save vm_state_save[1];
};

struct frame_input {
    struct frame base[1];
    sx_t file;
    struct {
        unsigned interactive : 1;
        unsigned eol : 1;
    } flags;
    unsigned depth;
    union {
        struct frame_input_interactive {
            char     *buf;      /* Returned from readline */
            unsigned ofs;       /* Offset into above */
        } interactive;
        struct frame_input_noninteractive {
            unsigned line_num, echo_ofs, echo_suppress_cnt;
        } noninteractive;
    } u;
};

struct vm {
    sx_t          *eval_stack, *eval_stack_top, *sp;
    unsigned char *frame_stack, *frame_stack_top;
    struct frame  *fp, *type_fp[NUM_FRAME_TYPES];
    unsigned      except_lvl;

#ifndef NDEBUG
    unsigned     debug_lvl;
#endif
};

static struct vm vm[1];

#define FUNCFP   ((struct frame_func *) vm->type_fp[FRAME_TYPE_FUNC])
#define ENVFP    ((struct frame_env *) vm->type_fp[FRAME_TYPE_ENV])
#define CLASSFP  ((struct frame_class *) vm->type_fp[FRAME_TYPE_CLASS])
#define XFP      ((struct frame_longjmp *) vm->type_fp[FRAME_TYPE_EXCEPT])
#define PROGFP   ((struct frame_longjmp *) vm->type_fp[FRAME_TYPE_PROG])
#define WHILEFP  ((struct frame_longjmp *) vm->type_fp[FRAME_TYPE_WHILE])
#define REPFP    ((struct frame_longjmp *) vm->type_fp[FRAME_TYPE_REP])
#define INFP     ((struct frame_input *) vm->type_fp[FRAME_TYPE_INPUT])

#define DST   (FUNCFP->dst)
#define ARGC  (FUNCFP->argc)
#define ARGS  (FUNCFP->args)

sx_t *vm_dst(void)
{
    return (DST);
}

unsigned vm_argc(void)
{
    return (ARGC);
}

sx_t vm_args(void)
{
    return (ARGS);
}

#ifndef NDEBUG

static inline void stack_depth_update(void)
{
    size_t n = vm->eval_stack_top - vm->sp;
    if (n > stack_stats.max_eval_stack_depth)  stack_stats.max_eval_stack_depth = n;
}

#endif

static const char eval_stack_overflow_mesg[] = "Eval stack overflow";

sx_t *eval_alloc(unsigned n)
{
    sx_t *result = vm->sp;
    
    vm->sp -= n;
    if (vm->sp < vm->eval_stack)  fatal(eval_stack_overflow_mesg);
    memset(vm->sp, 0, n * sizeof(*vm->sp));

#ifndef NDEBUG
    stack_depth_update();
#endif    
    
    return (result);
}


void eval_push(sx_t sx)
{
    --vm->sp;
    if (vm->sp < vm->eval_stack)  fatal(eval_stack_overflow_mesg);
    sx_retain(*vm->sp = sx);

#ifndef NDEBUG
    stack_depth_update();
#endif    
}


void eval_pop(unsigned n)
{
    for (; n > 0; --n) {
        if (vm->sp >= vm->eval_stack_top)  fatal("Eval stack underflow");
        sx_release(*vm->sp);
        ++vm->sp;
    }
}


void eval_unwind(sx_t *old)
{
    while (vm->sp < old) {
        sx_release(*vm->sp);
        ++vm->sp;
    }
}

struct frame *frame_push(unsigned type, unsigned size)
{
    unsigned char *p = (unsigned char *) vm->fp - size;
    if (p < vm->frame_stack)  fatal("Frame stack overflow");
    struct frame *fr = (struct frame *) p;
    fr->type = type;
    fr->prev = vm->fp;
    fr->type_prev = vm->type_fp[type];
    vm->fp = vm->type_fp[type] = fr;

#ifndef NDEBUG
    {
        size_t n = vm->frame_stack_top - p;
        if (n > stack_stats.max_frame_stack_depth)  stack_stats.max_frame_stack_depth = n;
    }
#endif
    
    return (fr);
}

#ifndef NDEBUG
void debug_leave(void);
#endif

void frame_pop1(void)
{
    struct frame *fr = vm->fp;
    switch (fr->type) {
#ifndef NDEBUG
    case FRAME_TYPE_FUNC:
        if (vm->except_lvl > 0)  break;
        debug_leave();
        break;
#endif

    case FRAME_TYPE_EXCEPT:
        if (vm->except_lvl > 0)  --vm->except_lvl;
        break;

    case FRAME_TYPE_MEM:
        {
            struct frame_mem *memfp = (struct frame_mem *) fr;
            if (memfp->data == 0)  break;
            mem_free(memfp->size, memfp->data);
        }
        break;

    default: ;
    }
    
    vm->fp = fr->prev;
    vm->type_fp[fr->type] = fr->type_prev;
}


static void frame_unwind(struct frame *fr)
{
    while (vm->fp != fr)  frame_pop1();
}


static inline void frame_pop(struct frame *fr)
{
    frame_unwind(fr->prev);
}


static inline void vm_state_save(struct vm_state_save *v)
{
    v->sp = vm->sp;
    v->fp = vm->fp;

#ifndef NDEBUG
    v->debug_lvl = vm->debug_lvl;
#endif
}


static inline void vm_state_restore(struct vm_state_save *v)
{
    frame_unwind(v->fp);
    eval_unwind(v->sp);
    /* N.B. Do not restore except_lvl */
    
#ifndef NDEBUG
    vm->debug_lvl = v->debug_lvl;
#endif
}

#ifndef NDEBUG

#include <signal.h>

char debug_breakpoint[80];
bool debug_stepf, debug_nextf, debug_tracef;
unsigned debug_next_lvl;

void debug_indent(void)
{
    unsigned k;
    for (k = vm->debug_lvl; k > 0; --k)  fputs("  ", stderr);
}


void debug_trace_enter(void)
{
    bool old = debug_tracef;
    debug_tracef = false;

    fputs("trace >> ", stderr);
    debug_indent();

    sx_t *work = eval_alloc(1);

    fputc('(', stderr);
    fputs(sx_repr(&work[-1], FUNCFP->func)->data, stderr);
    if (FUNCFP->args == 0) {
        fputs(")", stderr);
    } else {
        fputc(' ', stderr);
        fputs(sx_repr(&work[-1], FUNCFP->args)->data + 1, stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);

    eval_unwind(work);

    debug_tracef = old;
}


void debug_trace_leave(void)
{
    bool old = debug_tracef;
    debug_tracef = false;

    fputs("trace << ", stderr);
    debug_indent();

    sx_t *work = eval_alloc(1);

    fputs(sx_repr(&work[-1], *FUNCFP->dst)->data, stderr);
    fputc('\n', stderr);
    fflush(stderr);

    eval_unwind(work);

    debug_tracef = old;
}


static inline void debug_step(void)
{
    debug_stepf = true;
}


static inline void debug_next(void)
{
    debug_nextf    = true;
    debug_next_lvl = vm->debug_lvl;
}


void debug_enter(void)
{
    ++vm->debug_lvl;
}

static void debug_rep(void);

void debug_breakpoint_chk(void)
{
    sx_t x = FUNCFP->func;
    if ((sx_is_str(x) && strcmp(x->u.strval->data, debug_breakpoint) == 0)
        || debug_stepf
        || (debug_nextf && vm->debug_lvl <= debug_next_lvl)
        ) {
        debug_nextf = debug_stepf = false;
        
        debug_rep();
    }
}


void debug_trace(void)
{
    if (debug_tracef)  debug_trace_enter();
    debug_breakpoint_chk();
}


void debug_leave(void)
{
    if (debug_tracef)  debug_trace_leave();
    --vm->debug_lvl;
}


#define DEBUG_ENTER()           debug_enter()
#define DEBUG_BREAKPOINT_CHK()  debug_breakpoint_chk()
#define DEBUG_TRACE()           debug_trace()
#define DEBUG_LEAVE()           debug_leave()

#else

#define DEBUG_ENTER()
#define DEBUG_BREAKPOINT_CHK()
#define DEBUG_TRACE()
#define DEBUG_LEAVE()

#endif  /* !defined(NDEBUG) */

struct frame_func *frame_func_push(sx_t *dst, sx_t func, int argc, sx_t args)
{
    struct frame_func *fr = (struct frame_func *) frame_push(FRAME_TYPE_FUNC, sizeof(*fr));
    fr->dst  = dst;
    fr->func = func;
    fr->args = args;
    fr->argc = argc;

    DEBUG_ENTER();
    DEBUG_TRACE();
    
    return (fr);
}

struct frame_longjmp *frame_longjmp_push(unsigned type, sx_t *arg)
{
    struct frame_longjmp *fr = (struct frame_longjmp *) frame_push(type, sizeof(*fr));
    fr->jc  = FRAME_LONGJMP_JC_NONE;
    fr->arg = arg;
    vm_state_save(fr->vm_state_save);

    return (fr);
}


static inline void frame_longjmp_pop(struct frame_longjmp *fr)
{
    frame_pop(fr->base);
}

#define FRAME_LONGJMP_SETJMP(ljfp)  (setjmp((ljfp)->jb))

static inline void frame_longjmp_longjmp(struct frame_longjmp *ljfp, unsigned jc)
{
    vm_state_restore(ljfp->vm_state_save);

    ljfp->jc = jc;
    longjmp(ljfp->jb, 1);
}


static inline struct frame_longjmp *frame_rep_push(void)
{
    return (frame_longjmp_push(FRAME_TYPE_REP, 0));
}


static inline void frame_rep_restart(void)
{
    frame_longjmp_longjmp(REPFP, FRAME_LONGJMP_JC_REP_RESTART);
}


static inline void frame_rep_exit(void)
{
    frame_longjmp_longjmp(REPFP, FRAME_LONGJMP_JC_REP_EXIT);
}


static inline struct frame_longjmp *frame_except_push(sx_t *arg)
{
    return (frame_longjmp_push(FRAME_TYPE_EXCEPT, arg));
}

struct frame_longjmp *except_top;

static void frame_except_uncaught(void)
{
    fprintf(stderr, "Uncaught exception: ");
    sx_print(stderr, *XFP->arg);
    fputc('\n', stderr);
    backtrace();
    
    if (vm->except_lvl > 0) --vm->except_lvl;
}


static inline void frame_except_raise(void)
{
    if (XFP == except_top)  frame_except_uncaught();
    
    frame_longjmp_longjmp(XFP, FRAME_LONGJMP_JC_EXCEPT_RAISE);
}


static inline struct frame_longjmp *frame_prog_push(sx_t *arg)
{
    return (frame_longjmp_push(FRAME_TYPE_PROG, arg));
}


static inline void frame_prog_goto(sx_t label)
{
    sx_assign(PROGFP->arg, label);
    
    frame_longjmp_longjmp(PROGFP, FRAME_LONGJMP_JC_PROG_GOTO);
}


static inline void frame_prog_return(sx_t val)
{
    sx_assign(PROGFP->arg, val);
    
    frame_longjmp_longjmp(PROGFP, FRAME_LONGJMP_JC_PROG_RETURN);
}


static inline struct frame_longjmp *frame_while_push(void)
{
    return (frame_longjmp_push(FRAME_TYPE_WHILE, 0));
}


static inline void frame_while_continue(void)
{
    frame_longjmp_longjmp(WHILEFP, FRAME_LONGJMP_JC_WHILE_CONTINUE);
}


static inline void frame_while_break(void)
{
    frame_longjmp_longjmp(WHILEFP, FRAME_LONGJMP_JC_WHILE_BREAK);
}


struct frame_env *_frame_env_push(sx_t dict, sx_t module, struct frame_env *up)
{
    struct frame_env *fr = (struct frame_env *) frame_push(FRAME_TYPE_ENV, sizeof(*fr));
    fr->dict   = dict;
    fr->module = module;
    fr->up     = up;

    return (fr);
}

struct frame_env *env_global;

static inline struct frame_env *frame_env_push_module(sx_t dict, sx_t module)
{
    return (_frame_env_push(dict, module, module == 0 ? ENVFP : env_global));
}


static inline struct frame_env *frame_env_push(sx_t dict)
{
    return (_frame_env_push(dict, 0, ENVFP));
}

enum {
    NON_INTERACTIVE,
    INTERACTIVE
};    

struct frame_input *frame_input_push(sx_t file, bool interactivef)
{
    struct frame_input *fr = (struct frame_input *) frame_push(FRAME_TYPE_INPUT, sizeof(*fr));
    fr->file = file;
    fr->depth = 0;
    memset(&fr->u, 0, sizeof(fr->u));
    if ((fr->flags.interactive = interactivef)) {
        fr->flags.eol = false;;        
    } else {
        fr->u.noninteractive.line_num = 1;
    }

    return (fr);
}


void frame_input_reset(void)
{
    INFP->depth = 0;
  
    if (!INFP->flags.interactive)  return;

    INFP->flags.eol = false;
    if (INFP->u.interactive.buf == 0)  return;

    free(INFP->u.interactive.buf);
    INFP->u.interactive.buf = 0;
}


struct frame_mem *frame_mem_push(unsigned size, void *data)
{
    struct frame_mem *fr = (struct frame_mem *) frame_push(FRAME_TYPE_MEM, sizeof(*fr));
    fr->size = size;
    fr->data = data;
    
    return (fr);
}


static inline void frame_mem_mark_taken(struct frame_mem *fr)
{
    fr->data = 0;
}


struct frame_class *frame_class_push(sx_t class)
{
    struct frame_class *fr = (struct frame_class *) frame_push(FRAME_TYPE_CLASS, sizeof(*fr));
    fr->class = class;

    return (fr);
}

static void sym_collect(void);

static void collect(void)
{
    collectingf = true;
    
    sx_in_use_list_swap();

    struct dllist *p;
    for (p = dllist_first(sx_in_use_list_grey); p != dllist_end(sx_in_use_list_grey); p = p->next) {
        FIELD_PTR_TO_STRUCT_PTR(p, struct sx, list_node)->ref_cnt = 0;
    }

    /* Everything is kept on the stack, including:
       - environments
       - the symbol table
    */
    
    sx_t *q;
    for (q = vm->sp; q < vm->eval_stack_top; ++q)  sx_mark(*q);

    for (;;) {
        p = dllist_first(sx_in_use_list_grey);
        if (p == dllist_end(sx_in_use_list_grey))  break;
        sx_cleanup(FIELD_PTR_TO_STRUCT_PTR(p, struct sx, list_node));
    }

    sym_collect();
    
    collectingf = false;
}

/***************************************************************************
 *
 * Exceptions
 *
 ***************************************************************************/

void except_raise1(void)
{
    if (++vm->except_lvl <= 1)  return;
    
    fprintf(stderr, "Double exception\n");
    myexit(1);
}


void except_newv(unsigned type_size, const char *type_data, sx_t args)
{
    DEBUG_ASSERT(list_len(args) >= 0);

    sx_t *work = eval_alloc(1);

    cons(XFP->arg, str_newc(&work[-1], type_size, type_data), args);
}

void list_append(sx_t **p, sx_t item);

void except_newl(unsigned type_size, const char *type_data, unsigned argc, ...)
{
    sx_t *p;
    va_list ap;
    va_start(ap, argc);
    for (p = XFP->arg; argc > 0; --argc)  list_append(&p, va_arg(ap, sx_t));
    va_end(ap);

    except_newv(type_size, type_data, *XFP->arg);
}

/***************************************************************************
 *
 * Constructors
 *
 ***************************************************************************/

/* Naming convention for constructors:
   xxx_new() - default value, or simple init value
   xxx_newc() - create with initial data, data is copied
   xxx_newm() - create with initial data, data is buffer on heap and ownership is transferred
   xxx_newb() - create with initial data, data is borrowed
*/

enum { TAKE_OWNERSHIP = 0, BORROW };

sx_t bool_new(sx_t *dst, bool f)
{
    if (f) {
        sx_assign(dst, main_consts.t);
    } else {
        sx_assign_nil(dst);
    }

    return (*dst);
}


sx_t int_new(sx_t *dst, intval_t val)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_INT;
    sx->u.intval = val;
    sx_assign(dst, sx);

    return (sx);
}


sx_t float_new(sx_t *dst, floatval_t val)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_FLOAT;
    sx->u.floatval = val;
    sx_assign(dst, sx);

    return (sx);
}


sx_t complex_new(sx_t *dst, floatval_t re, floatval_t im)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_COMPLEX;
    sx->u.complexval->re = re;
    sx->u.complexval->im = im;
    sx_assign(dst, sx);

    return (sx);
}


sx_t numeric_new(sx_t *dst, struct sx_numeric *n)
{
    switch (n->type) {
    case SX_TYPE_INT:
        return (int_new(dst, n->u.intval));
    case SX_TYPE_FLOAT:
        return (float_new(dst, n->u.floatval));
        break;
    case SX_TYPE_COMPLEX:
        return (complex_new(dst, n->u.complexval->re, n->u.complexval->im));
    default:
        assert(0);
    }

    return (0);
}


bool numeric_from_sx(struct sx_numeric *n, sx_t sx)
{
    switch (n->type = sx->type) {
    case SX_TYPE_INT:
        n->u.intval = sx->u.intval;
        break;
    case SX_TYPE_FLOAT:
        n->u.floatval = sx->u.floatval;
        break;
    case SX_TYPE_COMPLEX:
        *n->u.complexval = *sx->u.complexval;
        break;
    default:
        return (false);
    }

    return (true);
}


void numeric_convert(struct sx_numeric *n, unsigned to_type)
{
    switch (n->type) {
    case SX_TYPE_INT:
        switch (to_type) {
        case SX_TYPE_INT:
            return;
        case SX_TYPE_FLOAT:
            n->u.floatval = (floatval_t) n->u.intval;
            break;
        case SX_TYPE_COMPLEX:
            n->u.complexval->re = (floatval_t) n->u.intval;
            n->u.complexval->im = 0.0;
            break;
        default:
            assert(0);
        }
        break;

    case SX_TYPE_FLOAT:
        switch (to_type) {
        case SX_TYPE_INT:
            n->u.intval = (intval_t) n->u.floatval;
            break;
        case SX_TYPE_FLOAT:
            return;
        case SX_TYPE_COMPLEX:
            n->u.complexval->re = n->u.floatval;
            n->u.complexval->im = 0.0;
            break;
        default:
            assert(0);
        }
        break;

    case SX_TYPE_COMPLEX:
        switch (to_type) {
        case SX_TYPE_COMPLEX:
            return;
        default:
            assert(0);
        }
        break;

    default:
        assert(0);
    }

    n->type = to_type;
}


sx_t str_new_type(sx_t *dst, unsigned type, unsigned size, const char *data, bool borrowf)
{
    DEBUG_ASSERT(size > 0);
    sx_t sx = sx_alloc();
    sx->type = type;
    struct sx_strval *s = sx->u.strval;
    s->size = size;
    s->data = data;
    sx->flags.mem_borrowed = borrowf;
    sx_assign(dst, sx);

    return (sx);
}


sx_t str_newc(sx_t *dst, unsigned size, const char *s)
{
    if (size == 0)  size = 1;
    char *p = mem_alloc(size);
    unsigned n = size - 1;
    memcpy(p, s, n);
    p[n] = 0;

    return (str_new_type(dst, SX_TYPE_STR, size, p, TAKE_OWNERSHIP));
}


sx_t str_newc1(sx_t *dst, const char *s)
{
    unsigned size = strlen(s) + 1;
    char *p = mem_alloc(size);
    memcpy(p, s, size);

    return (str_new_type(dst, SX_TYPE_STR, size, p, TAKE_OWNERSHIP));
}


sx_t str_newm(sx_t *dst, unsigned size, const char *s)
{
    DEBUG_ASSERT(strlen(s) == (size - 1));
    
    return (str_new_type(dst, SX_TYPE_STR, size, s, TAKE_OWNERSHIP));
}


sx_t str_newb(sx_t *dst, unsigned size, const char *s)
{
    return (str_new_type(dst, SX_TYPE_STR, size, s, BORROW));
}

struct sx_setval *sym_set;      /* Set of all defined symbols */
sx_t *set_finds(const struct sx_setval *s, unsigned size, const char *data, unsigned hash, sx_t **bucket);

static inline sx_t sym_new_chk(unsigned size, const char *s, unsigned hash)
{
    sx_t *p = set_finds(sym_set, size, s, hash, 0);
    return (p == 0 ? 0 : car(*p));
}


sx_t sym_newc(sx_t *dst, unsigned size, const char *s)
{
    unsigned h = str_hashc(size, s);
    sx_t sx = sym_new_chk(size, s, h);
    if (sx != 0) {
        sx_assign(dst, sx);

        return (sx);
    }

    char *p = mem_alloc(size);
    memcpy(p, s, size);
    sx = str_new_type(dst, SX_TYPE_SYM, size, p, TAKE_OWNERSHIP);
    sx->hash = h;
    sx->flags.hash_valid = true;
    sx->flags.permanent  = initf;
    set_puts(sym_set, sx, h);

    return (sx);
}


sx_t sym_newm(sx_t *dst, unsigned size, const char *s)
{
    unsigned h = str_hashc(size, s);
    sx_t sx = sym_new_chk(size, s, h);
    if (sx != 0) {
        mem_free(size, (void *) s);

        sx_assign(dst, sx);
        
        return (sx);
    }

    sx = str_new_type(dst, SX_TYPE_SYM, size, s, TAKE_OWNERSHIP);
    sx->hash = h;
    sx->flags.hash_valid = true;
    sx->flags.permanent  = initf;
    set_puts(sym_set, sx, h);

    return (sx);
}


sx_t sym_newb(sx_t *dst, unsigned size, const char *s)
{
    unsigned h = str_hashc(size, s);
    sx_t sx = sym_new_chk(size, s, h);
    if (sx != 0) {
        sx_assign(dst, sx);

        return (sx);
    }

    sx = str_new_type(dst, SX_TYPE_SYM, size, s, BORROW);
    sx->hash = h;
    sx->flags.hash_valid = true;
    sx->flags.permanent  = initf;
    set_puts(sym_set, sx, h);

    return (sx);
}


void sym_free(sx_t sx)
{
    set_dels(sym_set, sx, sx->hash);
}


void sym_collect(void)
{
    struct sx_arrayval *a = sym_set->base;
    sx_t *p;
    unsigned n;
    for (p = a->data, n = a->size; n > 0; --n, ++p) {
        sx_t q, next;
        for (q = *p; q != 0; q = next) {
            next = cdr(q);
            sx_t s = car(q);
            if (s->ref_cnt == 1 && !s->flags.permanent)  sym_free(s);
        }
    }
}


sx_t subr_new_type(sx_t *dst, unsigned type, codefuncval_t func, sx_t name)
{
    sx_t sx = sx_alloc();
    sx->type = type;
    struct sx_codefuncval *cf = sx->u.codefuncval;
    cf->func = func;
    sx_assign_norelease(&cf->name, name);
    sx_assign(dst, sx);

    return (sx);
}


sx_t subr_new(sx_t *dst, codefuncval_t func, sx_t name)
{
    return (subr_new_type(dst, SX_TYPE_SUBR, func, name));
}


sx_t nsubr_new(sx_t *dst, codefuncval_t func, sx_t name)
{
    return (subr_new_type(dst, SX_TYPE_NSUBR, func, name));
}


sx_t cons(sx_t *dst, sx_t car, sx_t cdr)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_DPTR;
    struct sx_dptrval *d = sx->u.dptrval;
    sx_assign_norelease(&d->car, car);
    sx_assign_norelease(&d->cdr, cdr);
    sx_assign(dst, sx);

    return (sx);
}


void list_append(sx_t **p, sx_t item)
{
    *p = &cons(*p, item, 0)->u.dptrval->cdr;
}


sx_t *list_slice_unsafe(sx_t *dst, sx_t li, unsigned ofs, unsigned len)
{
    sx_t *p;
    unsigned i, n;
    for (sx_assign_nil(p = dst), i = n = 0; li != 0 && (len == 0 || n < len); li = cdr(li), ++i) {
        if (i < ofs)  continue;
        
        if (li->type != SX_TYPE_DPTR) {
            sx_assign(p, li);
            break;
        }
        
        list_append(&p, car(li));
        ++n;
    }

    return (p);
}


static inline sx_t *list_copy_unsafe(sx_t *dst, sx_t li)
{
    return (list_slice_unsafe(dst, li, 0, 0));
}

static void sx_copydeep(sx_t *dst, sx_t sx);

sx_t *list_copydeep_unsafe(sx_t *dst, sx_t li)
{
    sx_t *work = eval_alloc(1);

    sx_t *p;
    for (sx_assign_nil(p = dst); li != 0; li = cdr(li)) {
        if (li->type != SX_TYPE_DPTR) {
            sx_copydeep(p, li);
            break;
        }

        sx_copydeep(&work[-1], car(li));
        list_append(&p, work[-1]);
    }

    eval_unwind(work);

    return (p);
}


sx_t file_new(sx_t *dst, sx_t filename, sx_t mode, FILE *fp)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_FILE;
    struct sx_fileval *f = sx->u.fileval;
    sx_assign_norelease(&f->filename, filename);
    sx_assign_norelease(&f->mode, mode);
    f->fp = fp;
    sx_assign(dst, sx);

    return (sx);    
}


FILE *fdup(FILE *fp, const char *mode)
{
    FILE *result = fdopen(dup(fileno(fp)), mode);
    fseek(result, ftell(fp), SEEK_SET);

    return (result);
}


sx_t file_copy(sx_t *dst, sx_t src)
{
    struct sx_fileval *f_src = src->u.fileval;
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_FILE;
    struct sx_fileval *f_dst = sx->u.fileval;
    sx_assign_norelease(&f_dst->filename, f_src->filename);
    sx_assign_norelease(&f_dst->mode, f_src->mode);
    f_dst->fp = fdup(f_src->fp, f_src->mode->u.strval->data);
    sx_assign(dst, sx);    

    return (*dst);
}


sx_t array_new_type(sx_t *dst, unsigned type, intval_t size, sx_t *data)
{
    sx_t sx = sx_alloc();
    sx->type = type;
    struct sx_arrayval *a = sx->u.arrayval;
    a->size = size;
    unsigned size_bytes = size * sizeof(a->data[0]);
    a->data = (sx_t *) mem_alloc(size_bytes);
    if (data == 0) {
        memset(a->data, 0, size_bytes);
    } else {
        sx_t *p;
        for (p = a->data; size > 0; --size, ++p, ++data) {
            sx_assign_norelease(p, *data);
        }
    }
    sx_assign(dst, sx);

    return (sx);    
}


sx_t array_new(sx_t *dst, intval_t size)
{
    return (array_new_type(dst, SX_TYPE_ARRAY, size, 0));
}


sx_t array_slice(sx_t *dst, sx_t src, unsigned ofs, unsigned len)
{
    return (array_new_type(dst, src->type, len, src->u.arrayval->data + ofs));
}


sx_t array_copy(sx_t *dst, sx_t src)
{
    struct sx_arrayval *a = src->u.arrayval;
    return (array_new_type(dst, src->type, a->size, a->data));
}


sx_t array_copydeep_unsafe(sx_t *dst, sx_t src)
{
    struct sx_arrayval *a_src = src->u.arrayval;
    unsigned n = a_src->size;
    struct sx_arrayval *a_dst = array_new_type(dst, src->type, n, 0)->u.arrayval;

    sx_t *p, *q;
    for (p = a_src->data, q = a_dst->data; n > 0; --n, ++p, ++q) {
        sx_copydeep(q, *p);
    }

    return (*dst);
}


sx_t barray_new_type(sx_t *dst, unsigned type, intval_t size, unsigned char *data, bool borrowf)
{
    sx_t sx = sx_alloc();
    sx->type = type;
    struct sx_barrayval *b = sx->u.barrayval;
    b->size = size;
    b->data = data;
    sx->flags.mem_borrowed = borrowf;
    sx_assign(dst, sx);

    return (sx);    
}


sx_t barray_new(sx_t *dst, intval_t size)
{
    return (barray_new_type(dst, SX_TYPE_BARRAY, size, mem_alloc(size), TAKE_OWNERSHIP));
}


sx_t barray_newc(sx_t *dst, intval_t size, unsigned char *data)
{
    unsigned char *p = (unsigned char *) mem_alloc(size);
    if (data == 0) {
        memset(p, 0, size);
    } else {
        memcpy(p, data, size);
    }
    
    return (barray_new_type(dst, SX_TYPE_BARRAY, size, p, TAKE_OWNERSHIP));
}


sx_t barray_newm(sx_t *dst, intval_t size, unsigned char *data)
{
    DEBUG_ASSERT(data != 0);

    return (barray_new_type(dst, SX_TYPE_BARRAY, size, data, TAKE_OWNERSHIP));
}


sx_t barray_newb(sx_t *dst, intval_t size, unsigned char *data)
{
    DEBUG_ASSERT(data != 0);
    
    return (barray_new_type(dst, SX_TYPE_BARRAY, size, data, BORROW));
}


sx_t barray_slice(sx_t *dst, sx_t src, unsigned ofs, unsigned len)
{
    unsigned char *p = (unsigned char *) mem_alloc(len);
    memcpy(p, src->u.barrayval->data + ofs, len);

    return (barray_new_type(dst, src->type, len, p, TAKE_OWNERSHIP));
}


static inline sx_t barray_copy(sx_t *dst, sx_t src)
{
    return (barray_slice(dst, src, 0, src->u.barrayval->size));
}


sx_t blob_new_init(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size, unsigned char *data, bool borrowf)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_BLOB;
    struct sx_blobval *b = sx->u.blobval;
    sx_assign_norelease(&b->inst_of, cl);
    b->hooks = hooks;
    b->size  = size;
    b->data  = data;
    sx->flags.mem_borrowed = borrowf;
    sx_assign(dst, sx);

    return (sx);
}


sx_t blob_new(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size)
{
    return (blob_new_init(dst, cl, hooks, size, mem_alloc(size), TAKE_OWNERSHIP));
}


sx_t blob_newc(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size, unsigned char *data)
{
    unsigned char *p = mem_alloc(size);
    if (data == 0) {
        memset(p, 0, size);
    } else {
        memcpy(p, data, size);
    }
    
    return (blob_new_init(dst, cl, hooks, size, data, TAKE_OWNERSHIP));
}


sx_t blob_newm(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size, unsigned char *data)
{
    return (blob_new_init(dst, cl, hooks, size, data, TAKE_OWNERSHIP));
}


sx_t blob_newb(sx_t *dst, sx_t cl, const struct blobhooks *hooks, unsigned size, unsigned char *data)
{
    return (blob_new_init(dst, cl, hooks, size, data, BORROW));
}

/* N.B. A blob must implement copy and copydeep methods, as required */

#define SET_SIZE_DEFAULT  16

sx_t set_new_type(sx_t *dst, unsigned type, unsigned size)
{
    size = (size == 0) ? SET_SIZE_DEFAULT : round_up_to_power_of_2(size);
    sx_t sx = array_new_type(dst, type, size, 0);
    sx->u.setval->cnt = 0;

    return (sx);    
}


static sx_t set_copy_unsafe_mode(sx_t *dst, sx_t src, sx_t *(*func)(sx_t *, sx_t))
{
    struct sx_setval   *s = src->u.setval;
    struct sx_arrayval *a = s->base;
    unsigned n = a->size;

    struct sx_setval *r = set_new_type(dst, src->type, n)->u.setval;

    sx_t *p, *q;
    for (p = r->base->data, q = a->data; n > 0; --n, ++p, ++q) {
        (*func)(p, *q);
    }
    r->cnt = s->cnt;

    return (*dst);
}


static inline sx_t set_copy_unsafe(sx_t *dst, sx_t src)
{
    return (set_copy_unsafe_mode(dst, src, list_copy_unsafe));
}


static inline sx_t set_copydeep_unsafe(sx_t *dst, sx_t src)
{
    return (set_copy_unsafe_mode(dst, src, list_copydeep_unsafe));
}


sx_t closure_new(sx_t *dst, sx_t expr, sx_t dict)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_CLOSURE;
    struct sx_closureval *cl = sx->u.closureval;
    sx_assign_norelease(&cl->expr, expr);
    sx_assign_norelease(&cl->dict, dict);
    sx_assign(dst, sx);

    return (sx);    
}


sx_t closure_copy_mode(sx_t *dst, sx_t src, sx_t (*func)(sx_t *, sx_t))
{
    struct sx_closureval *cl_src = src->u.closureval;
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_CLOSURE;
    struct sx_closureval *cl_dst = sx->u.closureval;
    sx_assign_norelease(&cl_dst->expr, cl_src->expr);
    cl_dst->dict = 0;
    (*func)(&cl_dst->dict, cl_src->dict);
    sx_assign(dst, sx);

    return (sx);
}


static inline sx_t closure_copy(sx_t *dst, sx_t src)
{
    return (closure_copy_mode(dst, src, set_copy_unsafe));
}


static inline sx_t closure_copydeep(sx_t *dst, sx_t src)
{
    return (closure_copy_mode(dst, src, set_copydeep_unsafe));
}


sx_t module_new(sx_t *dst, sx_t dict, sx_t name, sx_t sha1, void *dlhdl)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_MODULE;
    struct sx_moduleval *m = sx->u.moduleval;
    sx_assign_norelease(&m->name, name);
    sx_assign_norelease(&m->sha1, sha1);
    sx_assign_norelease(&m->dict, dict);
    m->dlhdl = dlhdl;
    sx_assign(dst, sx);

    return (sx);    
}


sx_t class_new_init(sx_t *dst, sx_t inst_of, sx_t name, sx_t parent)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_CLASS;
    sx_assign_norelease(&sx->u.classval->inst_of, inst_of);
    struct class *cl = (struct class *) mem_alloc(sizeof(*cl));
    memset(cl, 0, sizeof(*cl));
    sx_assign_norelease(&cl->name, name);
    sx_assign_norelease(&cl->parent, parent);
    sx->u.classval->class = cl;
    sx_assign(dst, sx);

    dict_new(&cl->class_vars, 16);
    dict_new(&cl->class_methods, 16);
    dict_new(&cl->inst_methods, 16);

    return (sx);
}


static inline sx_t class_new(sx_t *dst, sx_t name, sx_t parent)
{
    return (class_new_init(dst, main_consts.Metaclass, name, parent));
}


sx_t obj_new_init(sx_t *dst, sx_t inst_of)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_OBJ;
    struct sx_objval *obj = sx->u.objval;
    sx_assign_norelease(&obj->inst_of, inst_of);
    obj->dict = 0;
    sx_assign(dst, sx);

    return (sx);
}


sx_t obj_new(sx_t *dst, sx_t inst_of)
{
    sx_t sx = obj_new_init(dst, inst_of);

    dict_new(&sx->u.objval->dict, 16);

    return (sx);
}


sx_t method_new(sx_t *dst, sx_t class, sx_t func)
{
    sx_t sx = sx_alloc();
    sx->type = SX_TYPE_METHOD;
    struct sx_methodval *m = sx->u.methodval;
    sx_assign_norelease(&m->class, class);
    sx_assign_norelease(&m->func, func);
    sx_assign(dst, sx);

    return (sx);  
}


sx_t iter_new(sx_t *dst, unsigned type, sx_t sx, unsigned idx, sx_t li)
{
    sx_t result = sx_alloc();
    result->type = type;
    struct sx_iterval *i = result->u.iterval;
    sx_assign_norelease(&i->sx, sx);
    i->idx = idx;
    i->li  = li;
    sx_assign(dst, result);

    return (result);
}


sx_t iter_copy(sx_t *dst, sx_t src)
{
    sx_t result = sx_alloc();
    sx_retain((*result = *src).u.iterval->sx);
    sx_assign(dst, result);

    return (result);      
}


sx_t closuregen_new(sx_t *dst, sx_t closure)
{
    sx_t result = sx_alloc();
    result->type = SX_TYPE_CLOSUREGEN;
    struct sx_closuregen *g = result->u.closuregenval;
    sx_assign_norelease(&g->cl, closure);
    g->dict = 0;
    set_copydeep_unsafe(&g->dict, closure->u.closureval->dict);
    sx_assign(dst, result);

    return (result);
}

/***************************************************************************
 *
 * Method lookups & exection
 *
 ***************************************************************************/

bool method_find_type(sx_t *dst, sx_t sel, sx_t cl, unsigned ofs)
{
    for (; cl != 0; cl = cl->u.classval->class->parent) {
        sx_t pr = dict_ats((*(sx_t *)((unsigned char *) cl->u.classval->class + ofs))->u.setval, sel, sel->hash);
        if (pr != 0) {
            method_new(dst, cl, cdr(pr));
            
            return (true);
        }
    }

    return (false);
}


bool method_find(sx_t *dst, sx_t recvr, sx_t sel)
{
    return ((sx_type(recvr) == SX_TYPE_CLASS
             && method_find_type(dst, sel, recvr, OFS_CLASS_METHODS)
             )
            || method_find_type(dst, sel, sx_inst_of(recvr), OFS_INST_METHODS)
            );
}


void except_no_method(sx_t recvr, sx_t sel)
{
    except_raise1();

    sx_t *work = eval_alloc(1);

    cons(&work[-1], sx_inst_of(recvr), cons(&work[-1], sel, 0));
    
    except_newv(STR_CONST("system.no-method"), work[-1]);
    
    frame_except_raise();
}


void method_find_except(sx_t *dst, sx_t recvr, sx_t sel)
{
    if (method_find(dst, recvr, sel))  return;
    except_no_method(recvr, sel);
}

enum { ARGS_EVAL_SUPPRESS_ALL = -1 };

static void _dptr_call(sx_t *dst, sx_t form, unsigned argc, sx_t args, int args_eval_suppress_cnt);

/* - The basic unit of exection is a "function": a subr, an nsubr or a dptr (lambda,
 *   nlambda, or macro)
 * - A closure is a pair, of a function and a dictionary
 *   - When executing a closure, push dictionary on environment, call function
 * - A method is a pair, of a function and the class in which the method lookup
 *   succeeded
 *   - When executing a method, push class frame, call function
 *   - Note that when calling a method's function, evaluation of the first
 *     argument may need to be suppressed
 */

/* Eval logic
 * - if sx is a symbol,
 *   - if sx is not bound in environment,
 *     - throw exception (symbol not bound)
 *   - return bound value
 * - if sx is a dptr,
 *   - let f := car(sx)
 *   - if f is a symbol,
 *     - if f is bound,
 *       - let f := value of f
 *     - else if len(sx) < 2,
 *       - throw exception (no such function)
 *     - else
 *       - let r := cadr(sx)
 *       - if f[0] != '-',
 *         - let r := eval(r)
 *       - let m := method_lookup(r, f)
 *       - if m is nil,
 *         - throw exception (method not found)
 *       - let f := function(m)
 *   - return result of call f with args cdr(sx)
 * - return sx
 */

static void method_call_run(sx_t *dst, sx_t method, unsigned argc, sx_t args, int args_eval_suppress_cnt)
{
    /* Here, method will be the result of a lookup of a
       call-selector (call, call1, calln) on a callable object, 
       which is car(args).
       This is the bottom of the chain, it will be either an nsubr, a subr
       or a form.
     */
    
    struct vm_state_save vmsave[1];
    vm_state_save(vmsave);

    struct sx_methodval *m = method->u.methodval;
    frame_class_push(m->class);

    sx_t f = m->func;

    switch (sx_type(f)) {
    case SX_TYPE_NSUBR:
    case SX_TYPE_SUBR:
        {
            frame_func_push(dst, f, argc, args);

            (*f->u.codefuncval->func)();
        }
        break;

    case SX_TYPE_DPTR:
        _dptr_call(dst, f, argc, args, args_eval_suppress_cnt);

        break;
        
    default:
        assert(0);
    }

    vm_state_restore(vmsave);
}


static void method_call_apply(sx_t *dst, sx_t sel, unsigned argc, sx_t args, int args_eval_suppress_cnt)
{
    sx_t *work = eval_alloc(1);

    method_find_except(&work[-1], car(args), sel);
    method_call_run(dst, work[-1], argc, args, args_eval_suppress_cnt);
    
    eval_unwind(work);
}


static sx_t method_call1_internal(sx_t *dst, sx_t sel, sx_t sx) /* Internal use only, arg eval suppressed */
{
    sx_t *work = eval_alloc(2);
    
    method_find_except(&work[-1], sx, sel);
    cons(&work[-2], work[-1], cons(&work[-2], sx, 0));
    method_call_apply(dst, main_consts.calln, 2, work[-2], ARGS_EVAL_SUPPRESS_ALL);

    eval_unwind(work);

    return (*dst);
}


static sx_t method_call_internal(sx_t *dst, sx_t sel, unsigned argc, sx_t args) /* Internal use only, arg eval suppressed */
{
    sx_t *work = eval_alloc(1);
    
    method_find_except(&work[-1], car(args), sel);
    cons(&work[-1], work[-1], args);
    method_call_apply(dst, main_consts.calln, 1 + argc, work[-1], ARGS_EVAL_SUPPRESS_ALL);

    eval_unwind(work);

    return (*dst);
}

/***************************************************************************
 *
 * Miscellaneous
 *
 ***************************************************************************/

unsigned mem_hash(unsigned size, const unsigned char *data)
{
    return (crc32(-1, data, size));
}


unsigned str_hashc(unsigned size, const char *data)
{
    return (mem_hash(size - 1, (unsigned char *) data));
}

    
unsigned str_hash(struct sx_strval *s)
{
    return (str_hashc(s->size, s->data));
}


void sx_print(FILE *fp, sx_t sx);

void backtrace(void)
{
    fputs("Backtrace, most recent at top:\n", stderr);

    sx_t *work = eval_alloc(1);

    unsigned i = 0;
    struct frame *fr;
    for (fr = vm->fp; (unsigned char *) fr < vm->frame_stack_top; fr = fr->prev) {
        switch (fr->type) {
        case FRAME_TYPE_FUNC:
            {
                struct frame_func *p = (struct frame_func *) fr;
                fprintf(stderr, "%4u: %s(", i, sx_repr(&work[-1], p->func)->data);
                if (p->args == 0) {
                    fputs(")", stderr);
                } else {
                    fputs(sx_repr(&work[-1], p->args)->data + 1, stderr);
                }
                fputc('\n', stderr);
                ++i;
            }
            break;
            
        case FRAME_TYPE_INPUT:
            {
                struct frame_input *p = (struct frame_input *) fr;
                fputs("From ", stderr);
                if (p->flags.interactive) {
                    fputs("<interactive>", stderr);
                } else {
                    fprintf(stderr, "file %s line %u",
                            p->file->u.fileval->filename->u.strval->data,
                            p->u.noninteractive.line_num
                            );
                }
                fputc('\n', stderr);
            }
            break;

        default: ;
        }
    }

    eval_unwind(work);
}


static char system_bad_arg[] = "system.bad-argument";
static char type_mismatch[] = "type-mismatch";

void except_bad_arg(sx_t sx)
{
    except_raise1();
    
    except_newl(sizeof(system_bad_arg), system_bad_arg, 1, sx);

    frame_except_raise();
}

    
void except_bad_arg2(unsigned size, const char *s, sx_t sx)
{
    except_raise1();

    unsigned bufsize = sizeof(system_bad_arg) - 1 + 1 + size - 1 + 1;
    char buf[bufsize];
    snprintf(buf, bufsize, "%s.%s", system_bad_arg, s);

    except_newl(bufsize, buf, 1, sx);
    
    frame_except_raise();
}


bool str_equalc(struct sx_strval *s, unsigned size, const char *data)
{
    return (s->size == size && strncmp(s->data, data, size - 1) == 0);
}


bool str_equal(struct sx_strval *s1, struct sx_strval *s2)
{
    return (str_equalc(s1, s2->size, s2->data));
}


sx_t *set_find(const struct sx_setval *s, sx_t key, unsigned hash, sx_t **bucket)
{
    sx_t *work = eval_alloc(4);
    
    method_find_except(&work[-1], key, main_consts.equal);
    method_find_except(&work[-2], work[-1], main_consts.calln);
    const struct sx_arrayval *a = s->base;
    sx_t *p = &a->data[hash & (a->size - 1)], *result = 0;
    if (bucket != 0)  *bucket = p;
    sx_t q;
    for (; (q = *p) != 0; p = &q->u.dptrval->cdr) {
        cons(&work[-3], work[-1], cons(&work[-3], key, cons(&work[-3], car(q), 0)));
        method_call_run(&work[-4], work[-2], 3, work[-3], ARGS_EVAL_SUPPRESS_ALL);
        if (work[-4] != 0) {
            result = p;

            break;
        }
    }

    eval_unwind(work);
    
    return (result);
}


sx_t *set_finds(const struct sx_setval *s, unsigned size, const char *data, unsigned hash, sx_t **bucket)
{
    const struct sx_arrayval *a = s->base;
    sx_t *p = &a->data[hash & (a->size - 1)];
    if (bucket != 0)  *bucket = p;
    sx_t q;
    for (; (q = *p) != 0; p = &q->u.dptrval->cdr) {
        sx_t r = car(q);
        if (r->hash == hash && str_equalc(r->u.strval, size, data))  return (p);
    }

    return (0);
}


sx_t *set_find_eq(const struct sx_setval *s, sx_t key, unsigned hash, sx_t **bucket)
{
    const struct sx_arrayval *a = s->base;
    sx_t *p = &a->data[hash & (a->size - 1)];
    if (bucket != 0)  *bucket = p;
    sx_t q;
    for (; (q = *p) != 0; p = &q->u.dptrval->cdr) {
        if (car(q) == key)  return (p);
    }

    return (0);
}


bool set_at(const struct sx_setval *s, const sx_t key, unsigned hash)
{
    return (set_find(s, key, hash, 0) != 0);
}


void _set_put(struct sx_setval *s,
              sx_t *(*func)(const struct sx_setval *, sx_t, unsigned, sx_t **),
              sx_t key, unsigned hash
              )
{
    sx_t *bucket, *p = (*func)(s, key, hash, &bucket);
    if (p != 0)  return;
    
    ++s->cnt;
    assert(s->cnt > 0);

    cons(bucket, key , *bucket);
}


void set_put(struct sx_setval *s, sx_t key, unsigned hash)
{
    _set_put(s, set_find, key, hash);
}


void set_puts(struct sx_setval *s, sx_t key, unsigned hash)
{
    assert(sx_type(key) == SX_TYPE_SYM);

    _set_put(s, set_find_eq, key, hash);
}


void _set_del(struct sx_setval *s,
              sx_t *(*func)(const struct sx_setval *, sx_t, unsigned, sx_t **),
              sx_t key, unsigned hash
              )
{
    sx_t *p = (*func)(s, key, hash, 0);
    if (p == 0)  return;
    
    sx_assign(p, cdr(*p));
    assert(s->cnt > 0);
    --s->cnt;
}


void set_del(struct sx_setval *s, sx_t key, unsigned hash)
{
    _set_del(s, set_find, key, hash);
}


void set_dels(struct sx_setval *s, sx_t key, unsigned hash)
{
    _set_del(s, set_find_eq, key, hash);
}


sx_t *dict_find(const struct sx_setval *s, sx_t key, unsigned hash, sx_t **bucket)
{
    sx_t *work = eval_alloc(4);
    
    method_find_except(&work[-1], key, main_consts.equal);
    method_find_except(&work[-2], work[-1], main_consts.calln);
    const struct sx_arrayval *a = s->base;
    sx_t *p = &a->data[hash & (a->size - 1)], *result = 0;
    if (bucket != 0)  *bucket = p;
    sx_t q;
    for (; (q = *p) != 0; p = &q->u.dptrval->cdr) {
        cons(&work[-3], work[-1], cons(&work[-3], key, cons(&work[-3], car(car(q)), 0)));
        method_call_run(&work[-4], work[-2], 3, work[-3], ARGS_EVAL_SUPPRESS_ALL);
        if (work[-4] != 0) {
            result = p;

            break;
        }
    }

    eval_unwind(work);
    
    return (result);
}


sx_t *dict_find_eq(const struct sx_setval *s, sx_t key, unsigned hash, sx_t **bucket)
{
    const struct sx_arrayval *a = s->base;
    sx_t *p = &a->data[hash & (a->size - 1)];
    if (bucket != 0)  *bucket = p;
    sx_t q;
    for (; (q = *p) != 0; p = &q->u.dptrval->cdr) {
        if  (car(car(q)) == key)  return (p);
    }

    return (0);
}


sx_t _dict_at(const struct sx_setval *s,
              sx_t *(*func)(const struct sx_setval *, sx_t, unsigned, sx_t **),
              sx_t key, unsigned hash
              )
{
    sx_t *p = (*func)(s, key, hash, 0);
    return (p == 0 ? 0 : car(*p));
}


sx_t dict_at(const struct sx_setval *s, const sx_t key, unsigned hash)
{
    return (_dict_at(s, dict_find, key, hash));
}


sx_t dict_ats(const struct sx_setval *s, const sx_t key, unsigned hash)
{
    return (_dict_at(s, dict_find_eq, key, hash));
}


sx_t dict_at_dflt(const struct sx_setval *s, const sx_t key, unsigned hash, sx_t dflt)
{
    sx_t x = _dict_at(s, dict_find, key, hash);
    return (x == 0 ? dflt : cdr(x));
}


void _dict_atput(struct sx_setval *s,
                 sx_t *(*func)(const struct sx_setval *, sx_t, unsigned, sx_t **),
                 sx_t key, unsigned hash, sx_t value
                 )
{
    sx_t *bucket, *p = (*func)(s, key, hash, &bucket);
    if (p == 0) {
        ++s->cnt;
        assert(s->cnt > 0);
    } else {
        sx_assign(p, cdr(*p));
    }

    sx_t *work = eval_alloc(1);
    
    cons(bucket, cons(&work[-1], key, value), *bucket);
    
    eval_unwind(work);
}


void dict_atput(struct sx_setval *s, sx_t key, unsigned hash, sx_t value)
{
    _dict_atput(s, dict_find, key, hash, value);
}


void dict_atsput(struct sx_setval *s, sx_t key, unsigned hash, sx_t value)
{
    _dict_atput(s, dict_find_eq, key, hash, value);
}


void _dict_del(struct sx_setval *s,
               sx_t *(*func)(const struct sx_setval *, sx_t, unsigned, sx_t **),
               sx_t key, unsigned hash
               )
{
    sx_t *p = (*func)(s, key, hash, 0);
    if (p == 0)  return;
    
    sx_assign(p, cdr(*p));
    assert(s->cnt > 0);
    --s->cnt;
}


void dict_del(struct sx_setval *s, sx_t key, unsigned hash)
{
    _dict_del(s, dict_find, key, hash);
}


void dict_dels(struct sx_setval *s, sx_t key, unsigned hash)
{
    _dict_del(s, dict_find_eq, key, hash);
}


void except_lookup_non_symbol(sx_t sx)
{
    except_raise1();

    except_newl(STR_CONST("system.num-args"), 1, sx);

    frame_except_raise();
}


void except_symbol_not_bound(sx_t sym)
{
    except_raise1();

    except_newl(STR_CONST("system.not-bound"), 1, sym);

    frame_except_raise();
}


void except_no_environment(void)
{
    except_raise1();
    
    except_newv(STR_CONST("system.no-environment"), 0);

    frame_except_raise();
}


static inline struct frame_env *env_up_except(struct frame_env *e)
{
    struct frame_env *result = e->up;
    if (result == 0)  except_no_environment();
    
    return (result);
}


bool _env_find_from(struct frame_env **result_env, sx_t *result, sx_t sym, struct frame_env *e)
{
    assert(sym->flags.hash_valid);
    for (; e != 0; e = e->up) {
        sx_t *p = dict_find_eq(e->dict->u.setval, sym, sym->hash, 0);
        if (p != 0) {
            if (result_env != 0)  *result_env = e;
            *result = cdr(car(*p));
            
            return (true);
        }
    }

    return (false);
}


sx_t env_find_from(sx_t sym, struct frame_env *e)
{
    if (sx_type(sym) != SX_TYPE_SYM)  except_lookup_non_symbol(sym);
    sx_t result;
    if (_env_find_from(0, &result, sym, e))  return (result);

    except_symbol_not_bound(sym);

    return (0);
}


sx_t env_find(sx_t sym)
{
    return (env_find_from(sym, ENVFP));
}


void except_inv_op(void)
{
    except_raise1();
    
    except_newv(STR_CONST("system.invalid-operation"), 0);

    frame_except_raise();
}


void except_bind_non_symbol(sx_t sx)
{
    except_raise1();

    except_newl(STR_CONST("system.bind-non-symbol"), 1, sx);

    frame_except_raise();
}


void except_bind_const(sx_t sx)
{
    except_raise1();
    
    except_newl(STR_CONST("system.bind-const"), 1, sx);

    frame_except_raise();
}


void env_bind_dict(sx_t dict, sx_t sym, sx_t val)
{
    if (sx_type(sym) != SX_TYPE_SYM)  except_bind_non_symbol(sym);
    if (dict->flags.frozen)  except_bind_const(sym);

    assert(sym->flags.hash_valid);
    dict_atsput(dict->u.setval, sym, sym->hash, val);
}


void env_unbind_dict(sx_t dict, sx_t sym)
{
    if (sx_type(sym) != SX_TYPE_SYM)  except_bind_non_symbol(sym);
    if (dict->flags.frozen)  except_bind_const(sym);

    assert(sym->flags.hash_valid);
    dict_dels(dict->u.setval, sym, sym->hash);
}


void env_bind(sx_t sym, sx_t val)
{
    env_bind_dict(ENVFP->dict, sym, val);
}


void env_unbind(sx_t sym)
{
    env_unbind_dict(ENVFP->dict, sym);
}

/***************************************************************************
 *
 * Input parsing
 *
 ***************************************************************************/

void except_syntax_error(unsigned mesg_size,
                         const char *mesg,
                         unsigned str_size,
                         const char *str,
                         sx_t sx
                         )
{
    except_raise1();
    
    sx_t *work = eval_alloc(2);

    if (!INFP->flags.interactive) {
        str_newc1(&work[-1], INFP->file->u.fileval->filename->u.strval->data);
        int_new(&work[-2], INFP->u.noninteractive.line_num);
        cons(&work[-1], work[-1], work[-2]);
        cons(&work[-1], work[-1], 0);
    }
    if (sx != 0)  cons(&work[-1], sx, work[-1]);
    if (str_size > 0)  cons(&work[-1], str_newc(&work[-1], str_size, str), work[-1]);

    static const char ldr[] = "system.syntax-error.";
    unsigned bufsize = sizeof(ldr) - 1 + mesg_size - 1 + 1;
    char buf[bufsize];
    snprintf(buf, bufsize, "%s%s", ldr, mesg);

    except_newv(bufsize, buf, work[-1]);

    frame_except_raise();
}


void except_eof_in_input(void)
{
    except_raise1();
    
    except_newv(STR_CONST("system.unexpected-eof"), 0);

    frame_except_raise();
}


int sx_read_getc_interactive(void)
{
    struct frame_input_interactive *fr = &INFP->u.interactive;
    int c;
    for (;;) {
        if (fr->buf == 0) {
            rl_instream = INFP->file->u.fileval->fp;
            fr->buf = readline(INFP->depth == 0 ? "-> " : "");
            if (fr->buf == 0)  return (EOF);
            add_history(fr->buf);
            fr->ofs = 0;
        }
            
        c = fr->buf[fr->ofs];
        if (c != 0) {
            ++fr->ofs;

            return (c);
        }
        if (!INFP->flags.eol) {
            INFP->flags.eol = true;

            return ('\n');
        }

        INFP->flags.eol = false;
        
        free(fr->buf);
        fr->buf = 0;
    }
}


int sx_read_getc_noninteractive(void)
{
    int c = getc(INFP->file->u.fileval->fp);
    struct frame_input_noninteractive *fr = &INFP->u.noninteractive;

    do {
        if (fr->echo_suppress_cnt > 0) {
            --fr->echo_suppress_cnt;
            break;
        }
        
        if (echof) {
            if (fr->echo_ofs == 0)  fprintf(stderr, "\n%6d: ", fr->line_num);
            if (c != '\n')  fputc(c, stderr);
        }
        
        if (c == '\n') {
            ++fr->line_num;
            fr->echo_ofs = 0;
        } else {
            ++fr->echo_ofs;
        }        
    } while (0);
    
    return (c);
}

#ifdef TEST

int sx_read_getc_test(void)
{
  return (getc(INFP->file->u.fileval->fp));
}

#endif

int sx_read_getc(void)
{
#ifdef TEST
    return (sx_read_getc_test());
#else
    return (INFP->flags.interactive
            ? sx_read_getc_interactive() : sx_read_getc_noninteractive()
            );
#endif
}


void sx_read_ungetc(char c)
{
    if (INFP->flags.interactive) {
        struct frame_input_interactive *p = &INFP->u.interactive;
        DEBUG_ASSERT(p->ofs > 0);
        --p->ofs;
    } else {
        ungetc(c, INFP->file->u.fileval->fp);
        ++INFP->u.noninteractive.echo_suppress_cnt;
    }
}


int sx_read_getc_skip_comments(void)
{
    int c;
    for (;;) {
        c = sx_read_getc();
        if (c != ';')  break;
        do {
            c = sx_read_getc();
        } while (c != '\n');
    }

    return (c);
}


void *ebuf_push(unsigned capacity)
{
    struct ebuf *eb = (struct ebuf *) frame_push(FRAME_TYPE_MEM, sizeof(*eb));
    if (capacity == 0)  capacity = 256;
    struct frame_mem *fr = eb->base;
    fr->size = capacity;
    fr->data = mem_alloc(capacity);
    eb->ofs = 0;
    
    return (eb);
}


void ebuf_append(void *cookie, unsigned n, const char *data)
{
    struct ebuf *eb = (struct ebuf *) cookie;
    struct frame_mem *fr = eb->base;
    DEBUG_ASSERT(fr->base->type == FRAME_TYPE_MEM);
    unsigned newsize = eb->ofs + n;
    if (newsize > fr->size) {
        newsize = round_up_to_power_of_2(newsize);
        fr->data = mem_realloc(fr->data, fr->size, newsize);
        fr->size = newsize;
    }

    memcpy(fr->data + eb->ofs, data, n);
    eb->ofs += n;
}


void ebuf_appendc(void *cookie, const char c)
{
    ebuf_append(cookie, 1, &c);
}


const char *ebuf_data(void *cookie)
{
    return ((const char *)((struct ebuf *) cookie)->base->data);
}


unsigned ebuf_size(void *cookie)
{
    return (((struct ebuf *) cookie)->ofs);
}


sx_t _str_neweb(sx_t *dst, unsigned type, struct ebuf *eb)
{
    struct frame_mem *fr = eb->base;
    DEBUG_ASSERT(fr->base->type == FRAME_TYPE_MEM);
    if (eb->ofs != 0 && eb->ofs != fr->size) {
        fr->data = mem_realloc(fr->data, fr->size, eb->ofs);
        fr->size = eb->ofs;
    }

    sx_t result = str_new_type(dst, type, eb->ofs, (const char *) fr->data, TAKE_OWNERSHIP);

    frame_mem_mark_taken(fr);

    return (result);
}


sx_t str_neweb(sx_t *dst, void *cookie)
{
    return (_str_neweb(dst, SX_TYPE_STR, (struct ebuf *) cookie));
}


sx_t sym_neweb(sx_t *dst, void *cookie)
{
    return (_str_neweb(dst, SX_TYPE_SYM, (struct ebuf *) cookie));
}


static inline void ebuf_pop(void *cookie)
{
    frame_pop(((struct ebuf *) cookie)->base->base);
}


char *sx_read_str(void *cookie)
{
    struct ebuf *eb = (struct ebuf *) cookie;
    int c;
    do {
        c = sx_read_getc();
        if (c == EOF)  except_eof_in_input();
        if (isspace(c)) {
            c = 0;
        } else if (c == ')') {
            if (INFP->depth == 0)  except_syntax_error(STR_CONST("mismatched-parentheses"), 0, 0, 0);
            
            sx_read_ungetc(c);
            c = 0;
        }
        ebuf_appendc(eb, c);
    } while (c != 0);

    return ((char *) ebuf_data(eb));
}


static void trim(unsigned *size, const char **data) /* Size not including null terminator */
{
    unsigned n = *size;
    const unsigned char *p = (unsigned char *) *data;
    for ( ; n > 0; --n, ++p) {
        if (!isspace(*p))  break;
    }
    *data = (char *) p;
    for (p += n - 1; n > 0; --n, --p) {
        if (!isspace(*p))  break;
    }
    *size = n;
}


static bool int_from_str(intval_t *val, unsigned size, const char *data) /* size and data include null terminator */
{
    if (size >= 4 && data[0] == '0' && tolower(data[1]) == 'b') {
        if ((size -= (2 + 1)) > (8 * sizeof(*val)))  return (false);
        for (*val = 0, data += 2; size > 0; ++data) {
            --size;
            switch (*data) {
            case '1':
                *val |= 1 << size;
                break;
            case '0':
                break;
            default:
                return (false);
            }
        }

        return (true);
    }

    int ofs;
    return (sscanf(data, "%lli%n", val, &ofs) == 1
            && ofs == (size - 1)
            );
}


static bool float_from_str(floatval_t *val, unsigned size, const char *data)
{
    int ofs;
    return (sscanf(data, "%Lg%n", val, &ofs) == 1
            && ofs == (size - 1)
            );
}


static bool complex_from_str(floatval_t *re, floatval_t *im, unsigned size, const char *data)
{
    --size;
    int ofs;
    if (sscanf(data, "%Lg%n", re, &ofs) == 1
        && ofs == size
        ) {
        *im = 0.0;

        return (true);
    }
    char c1;
    if (sscanf(data, "%Lg%c%n", im, &c1, &ofs) == 2
        && ofs == size
        && c1 == 'i'
        ) {
        *re = 0.0;

        return (true);
    }
    char c2;
    int ofs2;
    if (sscanf(data, "%Lg%c%n%Lg%c%n", re, &c1, &ofs2, im, &c2, &ofs) == 4
        && ofs == size
        && (c1 == '+' || c1 == '-')
        && (!(data[ofs2] == '+' || data[ofs2] == '-'))
        && c2 == 'i'
        ) {
        if (c1 == '-')  *im = -*im;
        
        return (true);
    }

    return (false);
}


static bool numeric_from_str(struct sx_numeric *n, unsigned size, const char *data)
{
    if (int_from_str(&n->u.intval, size, data)) {
        n->type = SX_TYPE_INT;

        return (true);
    }
    if (float_from_str(&n->u.floatval, size, data)) {
        n->type = SX_TYPE_FLOAT;

        return (true);
    }
    if (complex_from_str(&n->u.complexval->re, &n->u.complexval->im, size, data)) {
        n->type = SX_TYPE_COMPLEX;

        return (true);
    }

    return (false);
}

/* <test> **********************************************************************
> ;; Test read parsing
> ;; Test read comments
> ;; This is a comment
> ;; Test read integerso
> 42
< 42
> ;; Test ignore spaces
>     99
< 99
> ;; Test signs
> +78
< 78
> -99
< -99
> ;; Test different input bases
> 0b10011
< 19
> 013
< 11
> 0x13
< 19
> ;; Test read floats
> 3.14
< 3.14
> ;; Test signs
> +9.87
< 9.87
> -1.23
< -1.23
> ;; Test scientific notation
> 6.789e1
< 67.89
> 2.718e+1
< 27.18
> -4501e-2
< -45.01
> ;; Test read complex
> 1.1+2.3i
< 1.1+2.3i
> -3-4.0i
< -3-4i
> ;; Test read strings
> "The rain in Spain"
< "The rain in Spain"
> "ab\ncde"
< "ab
< cde"
> ;; Test read symbols
> t
< t
>   nil
< nil
> ;; Test read lists
> ()
< nil
> '( 1 .    2)
< (1 . 2)
> '(1 2 3 . 4)
< (1 2 3 . 4)
> '(1 (2 3) 4)
< (1 (2 3) 4)
> ;; Test read objects
> 'a.b
< (ate a (<main.Nsubr: quote> b))
> 'a.b.c
< (ate (ate a (<main.Nsubr: quote> b)) (<main.Nsubr: quote> c))
> '(a.b := c)
< (atput a (<main.Nsubr: quote> b) c)
> '(a.b.c := d)
< (atput (ate a (<main.Nsubr: quote> b)) (<main.Nsubr: quote> c) d)
** </test> **********************************************************************/

static inline void sym_obj_parse_bad_object(unsigned str_size, const char *str)
{
    except_syntax_error(STR_CONST("bad-object"), str_size, str, 0);    
}


void sym_obj_parse(sx_t *dst, sx_t sx)
{
    struct sx_strval *s = sx->u.strval;
    const char *p = s->data;
    unsigned n = s->size - 1;
    
    sx_t *work = eval_alloc(3);
    
    while (n > 0) {
        char *q = index(p, '.');
        unsigned k = (q == 0) ? n : q - p;
        if (k == 0)  sym_obj_parse_bad_object(s->size, s->data);

        if (work[-1] == 0) {
            sym_newc(&work[-1], k + 1, p);
        } else {
            sym_newc(&work[-3], k + 1, p);
            cons(&work[-1], main_consts.ate,
                 cons(&work[-1], work[-1],
                      cons(&work[-3],
                           cons(&work[-3],
                                main_consts.quote,
                                cons(&work[-3], work[-3], 0)
                                ),
                           0
                           )
                      )
                 );
        }

        if (q != 0)  ++k;
        p += k;
        n -= k;
        if (q != 0 && n == 0)  sym_obj_parse_bad_object(s->size, s->data);
    }

    sx_move(dst, &work[-1]);
    
    eval_unwind(work);
}


static inline void sx_read_parse_assign_bad_assignment(sx_t x)
{
    except_syntax_error(STR_CONST("bad-assignment"), 0, 0, x);
}

bool sx_read_parse_assign(sx_t *dst, sx_t sx)
{
    if (!list_len_chk(sx, 3))  return (false);
    sx_t sel = cadr(sx);
    if (sx_type(sel) != SX_TYPE_SYM)  return (false);
    struct sx_strval *s = sel->u.strval;
    bool deff = str_equalc(s, STR_CONST("::="));
    if (!(deff || str_equalc(s, STR_CONST(":="))))  return (false);
    sx_t recvr = car(sx), val = car(cdr(cdr(sx)));

    sx_t *work = eval_alloc(1);

    cons(&work[-1], val, 0);
    if (deff) {
        cons(&work[-1],
             cons(&work[-1], main_consts.quote, work[-1]),
             0
             );
    }
        
    switch (sx_type(recvr)) {
    case SX_TYPE_SYM:
        {
            cons(dst, main_consts.setq,
                      cons(&work[-1], recvr, work[-1])
                 );
        }
        break;

    case SX_TYPE_DPTR:
        {
            if (!list_len_chk(recvr, 3)
                || car(recvr) != main_consts.ate) {
                sx_read_parse_assign_bad_assignment(recvr);
            }

            cons(dst, main_consts.atput,
                      cons(&work[-1], cadr(recvr),
                                      cons(&work[-1], car(cdr(cdr(recvr))),
                                                      work[-1]
                                           )
                           )
                 );
        }
        break;

    default:
        sx_read_parse_assign_bad_assignment(recvr);
    }

    eval_unwind(work);
    
    return (true);
}


static inline void sx_read_bad_dotted_pair(void)
{
    except_syntax_error(STR_CONST("bad-dotted-pair"), 0, 0, 0);
}


bool sx_read(sx_t *dst)
{
    char c;

    for (;;) {
        do {
            c = sx_read_getc();
            if (c == EOF)  return (false);
        } while (isspace(c));
        
        if (c != ';')  break;

        do {
            c = sx_read_getc();
            if (c == EOF)  return (false);
        } while (c != '\n');
    }

    if (c == '(') {
        ++INFP->depth;

        sx_t *work = eval_alloc(2), *p = &work[-1];
        
        bool dot_valid = false;
        for (;;) {
            do {
                c = sx_read_getc_skip_comments();
                if (c == EOF)  except_eof_in_input();
            } while (isspace(c));
            
            if (c == ')')  break;
            if (c == '.') {
                if (!dot_valid)  sx_read_bad_dotted_pair();
                if (!sx_read(p))  except_eof_in_input();
                c = sx_read_getc_skip_comments();
                if (c == EOF)  except_eof_in_input();
                if (c != ')')  sx_read_bad_dotted_pair();

                break;
            }
            sx_read_ungetc(c);
            if (!sx_read(&work[-2]))  except_eof_in_input();
            list_append(&p, work[-2]);
            dot_valid = true;
        }

        sx_t y;
        if (list_len_chk(work[-1], 3)
            && sx_type(y = cadr(work[-1])) == SX_TYPE_SYM
            && strcmp(y->u.strval->data, ":=") == 0
            ) {
            if (!sx_read_parse_assign(dst, work[-1])) {
                except_syntax_error(STR_CONST("invalid assignment"), 0, 0, 0);
            }
        } else {
            sx_move(dst, &work[-1]);
        }

        eval_unwind(work);

        --INFP->depth;
    } else if (c == ')') {
        except_syntax_error(STR_CONST("mismatched-parentheses"), 0, 0, 0);
    } else if (c == '\'') {
        ++INFP->depth;
        
        sx_t *work = eval_alloc(1);

        if (!sx_read(&work[-1]))  except_eof_in_input();
        cons(dst, main_consts.quote, cons(&work[-1], work[-1], 0));

        eval_unwind(work);

        --INFP->depth;
    } else if (c == '"') {
        ++INFP->depth;
        
        struct ebuf *eb = ebuf_push(32);

        do {
            c = sx_read_getc();
            if (c == EOF)  except_eof_in_input();
            if (c == '\\') {
                c = sx_read_getc();
                if (c == EOF)  except_eof_in_input();
                switch (c) {
                case 'r':  c = '\r';  break;
                case 'n':  c = '\n';  break;
                case 't':  c = '\t';  break;
                case 'x':
                    {
                        c = 0;
                        unsigned k;
                        for (k = 2; k > 0; --k) {
                            char d = sx_read_getc();
                            if (d == EOF)  except_eof_in_input();
                            if (!isxdigit(d))  except_syntax_error(STR_CONST("bad-hex-escape"), 2, &d, 0);
                            if (d <= '9') {
                                d -= '0';
                            } else  d = toupper(d) - 'A' + 10;
                            c = (c << 4) + d;
                        }
                    }
                    break;
                    
                default: ;
                }
            }
            else if (c == '"') {
                c = 0;
            }
            ebuf_appendc(eb, c);
        } while (c != 0);

        str_neweb(dst, eb);

        ebuf_pop(eb);

        --INFP->depth;
    } else {
        sx_read_ungetc(c);

        void *cookie = ebuf_push(32);

        sx_read_str(cookie);
        struct sx_numeric n[1];
        if (numeric_from_str(n, ebuf_size(cookie), ebuf_data(cookie))) {
            numeric_new(dst, n);
        } else {
            sym_neweb(dst, cookie);
            sym_obj_parse(dst, *dst);
        }

        ebuf_pop(cookie);
    }

    return (true);
}

/***************************************************************************
 *
 * Miscellaneous
 *
 ***************************************************************************/

void except_num_args(unsigned expected)
{
    except_raise1();
    
    sx_t *work = eval_alloc(2);

    cons(&work[-1], int_new(&work[-2], expected),
         cons(&work[-1], int_new(&work[-2], ARGC), 0)
         );
    except_newv(STR_CONST("system.num-args"), work[-1]);

    frame_except_raise();
}


void except_num_args_min(unsigned min)
{
    except_raise1();
    
    sx_t *work = eval_alloc(2);

    cons(&work[-1], int_new(&work[-2], min),
         cons(&work[-1], int_new(&work[-2], ARGC), 0)
         );
    except_newv(STR_CONST("system.num-args-min"), work[-1]);

    frame_except_raise();
}


void except_num_args_range(unsigned min, unsigned max)
{
    except_raise1();
    
    sx_t *work = eval_alloc(2);

    cons(&work[-1], int_new(&work[-2], min),
         cons(&work[-1], int_new(&work[-2], max),
              cons(&work[-1], int_new(&work[-2], ARGC), 0)
              )
         );
    except_newv(STR_CONST("system.num-args-range"), work[-1]);

    frame_except_raise();
}


void debug_mesg(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    vfprintf(stderr, fmt, ap);
    
    va_end(ap);

    fflush(stderr);
}


struct sx_strval *sx_inst_of_class_name(sx_t sx)
{
    return (sx_inst_of(sx)->u.classval->class->name->u.strval);
}


void except_bad_value(sx_t x)
{
    except_raise1();
    
    except_newl(STR_CONST("system.bad-value"), 1, x);

    frame_except_raise();
}


void sx_eval(sx_t *dst, sx_t sx)
{
    method_call1_internal(dst, main_consts.eval, sx);
}


static inline void sx_tolist(sx_t *dst, sx_t sx)
{
    method_call1_internal(dst, main_consts.tolist, sx);
}


static inline void sx_togenerator(sx_t *dst, sx_t sx)
{
    method_call1_internal(dst, main_consts.togenerator, sx);
}


static void sx_next(sx_t *dst, sx_t sx)
{
    method_call1_internal(dst, main_consts.next, sx);
    sx_t x = *dst;
    if (!(x == 0 || (sx_type(x) == SX_TYPE_DPTR && cdr(x) == 0))) {
        except_bad_value(x);
    }
}


static struct sx_strval *circular_ref(sx_t *dst, sx_t sx)
{
    static const char s2[] = "<circular-ref>";
    struct sx_strval *s = sx_inst_of(sx)->u.classval->class->name->u.strval;
    unsigned bufsize = 1 + s->size - 1 + (sx->flags.frozen ? 1 : 0) + 2 + sizeof(s2) - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no chance of exceptions */
    snprintf(buf, bufsize, "<%s%s: %s>",
             s->data,
             (sx->flags.frozen ? "!" : ""),
             s2
             );
    str_newm(dst, bufsize, buf);

    return ((*dst)->u.strval);
}


struct sx_strval *sx_repr(sx_t *dst, sx_t sx)
{
    if (sx != 0) {
        if (sx->flags.visited) { /* TODO: This won't catch user-defined types */
            return (circular_ref(dst, sx));
        }

        sx->flags.visited = true;
    }

    method_call1_internal(dst, main_consts.repr, sx);
    if (!sx_is_str(*dst))  except_bad_value(*dst);
    
    if (sx != 0)  sx->flags.visited = false;

    return ((*dst)->u.strval);
}


void sx_tostring(sx_t *dst, sx_t sx)
{
    if (sx != 0) {
        if (sx->flags.visited) { /* TODO: This won't catch user-defined types */
            circular_ref(dst, sx);
            
            return;
        }

        sx->flags.visited = true;
    }

    method_call1_internal(dst, main_consts.tostring, sx);
    if (!sx_is_str(*dst))  except_bad_value(*dst);

    if (sx != 0)  sx->flags.visited = false;        
}


unsigned sx_hash(sx_t sx)    /* Internal use only, arg eval suppressed */
{
    if (sx != 0 && sx->flags.hash_valid)  return (sx->hash);

    sx_t *work = eval_alloc(1);

    method_call1_internal(&work[-1], main_consts.hash, sx);
    if (sx_type(work[-1]) != SX_TYPE_INT)  except_bad_value(work[-1]);
    unsigned result = work[-1]->u.intval;
    
    eval_unwind(work);
    
    if (sx != 0) {
        sx->hash = result;
        sx->flags.hash_valid = true;
    }

    return (result);
}


bool sx_equal(sx_t sx1, sx_t sx2)       /* Internal use only, arg eval suppressed */
{
    sx_t *work = eval_alloc(2);

    cons(&work[-1], sx1, cons(&work[-1], sx2, 0));
    method_call_internal(&work[-2], main_consts.equal, 2, work[-1]);
    bool result = (work[-2] != 0);
    
    eval_unwind(work);

    return (result);
}


static int sx_cmp(sx_t sx1, sx_t sx2)   /* Internal use only, arg eval suppressed */
{
    sx_t *work = eval_alloc(2);

    cons(&work[-1], sx1, cons(&work[-1], sx2, 0));
    method_call_internal(&work[-2], main_consts.cmp, 2, work[-1]);
    if (sx_type(work[-2]) != SX_TYPE_INT)  except_bad_value(work[-2]);
    int result = work[-2]->u.intval;
    
    eval_unwind(work);

    return (result);
}


static int sx_cmp_with_callable(sx_t a, sx_t b, sx_t cmp)
{
    if (cmp == 0)  return (sx_cmp(a, b));

    sx_t *work = eval_alloc(2);

    cons(&work[-1], cmp, cons(&work[-1], a, cons(&work[-1], b, 0)));
    method_call_apply(&work[-2], main_consts.calln, 3, work[-1], ARGS_EVAL_SUPPRESS_ALL);
    if (sx_type(work[-2]) != SX_TYPE_INT)  except_bad_value(work[-2]);
    int result = work[-2]->u.intval;
    
    eval_unwind(work);

    return (result);
}


void sx_print(FILE *fp, sx_t sx)
{
    sx_t *work = eval_alloc(1);

    sx_tostring(&work[-1], sx);
    fputs(work[-1]->u.strval->data, fp);

    eval_unwind(work);
}

/* Convert to a frozen array */

void sx_toarray(sx_t *dst, sx_t sx)
{
    switch (sx_type(sx)) {
    case SX_TYPE_STR:
    case SX_TYPE_SYM:
        break;

    case SX_TYPE_BARRAY:
        break;

    case SX_TYPE_ARRAY:
        if (sx->flags.frozen) {
            sx_assign(dst, sx);

            break;
        }

        /* sx_copy_deep(dst, sx); */
        /* (*dst)->flags.frozen = true; */
        
        break;
        
    case SX_TYPE_SET:
        break;
        
    case SX_TYPE_DICT:
        break;

    case SX_TYPE_DPTR:
        break;

    case SX_TYPE_BLOB:
        break;

    default:
        except_bad_arg(sx);
    }
}

/***************************************************************************
 *
 * Functions and methods
 *
 ***************************************************************************/

/* Notes for codefunctions (cf_...)

   (1) If the cf is a subr, it is permitted to write to DST directly -- since
       the args were evalled and placed on the stack, they can never conflict
       with DST.
*/


/* <doc> **********************************************************************
## quote
### Type
nsubr
### Form
(quote _sexpr_)
### Description
Returns _sexpr_, unevaluated.
### Return value
Same as first argument
### Exceptions
None
### See also
### Examples
> -> (quote foo)  
> foo
** </doc> **********************************************************************/

/* <test> **********************************************************************
> ;; Test quote
> 'foo
< foo
> (quote bar)
< bar
> ''abc
< (<main.Nsubr: quote> abc)
** </test> **********************************************************************/

static void cf_quote(void)
{
    cf_argc_chk(1);

    sx_assign(DST, car(ARGS));
}

/* <doc> **********************************************************************
## lambda
### Type
nsubr
### Form
(lambda ...)
### Description
When the form
```
(lambda ...)
```
is itself evaluated, it returns itself.  This is a convenience, so that when
a lambda-expression is passed as an argument, it won't need to be quoted,
if the argument is evaluated.
### Return value
Entire form
### Exceptions
None
### See also
nlambda
### Examples
> -> (lambda (x) y)
> (lambda (x) y)
** </doc> **********************************************************************/

/* <test> **********************************************************************
> ;; Test lambda
> (lambda (a b) (add a b))
< (lambda (a b) (add a b))
** </test> **********************************************************************/

static void cf_lambda(void)
{
    cons(DST, main_consts.lambda, ARGS);
}

static void cf_function(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);

    do {
        if (!list_len_chk_min(x, 2))  break;
        sx_t y = car(x);
        if (!(y == main_consts.lambda
              || y == main_consts.nlambda
              || y == main_consts.macro
              )
            ) {
            break;
        }

        cons(DST, main_consts.function, ARGS);

        return;
    } while (0);

    sx_eval(DST, x);
}

/* <doc> **********************************************************************
## nlambda
### Type
nsubr
### Form
(nlambda ...)
### Description
When the form
```
(nlambda ...)
```
is itself evaluated, it returns itself.  This is a convenience, so that when
a nlambda-expression is passed as an argument, it won't need to be quoted,
if the argument is evaluated.
### Return value
Entire form
### Exceptions
None
### See also
lambda
### Examples
> -> (nlambda x y)
> (nlambda x y)
** </doc> **********************************************************************/

/* <test> **********************************************************************
> ;; Test nlambda
> (nlambda x (add a b))
< (nlambda x (add a b))
** </test> **********************************************************************/

static void cf_nlambda(void)
{
    cons(DST, main_consts.nlambda, ARGS);
}

/* <doc> **********************************************************************
## macro
### Type
nsubr
### Form
(macro ...)
### Description
When the form
```
(macro ...)
```
is itself evaluated, it returns itself.  This is a convenience, so that when
a macro-expression is passed as an argument, it won't need to be quoted,
if the argument is evaluated.
### Return value
Entire form
### Exceptions
None
### See also
lambda, nlambda
### Examples
> -> (macro x y)
> (macro x y)
** </doc> **********************************************************************/

/* <test> **********************************************************************
> ;; Test macro
> (macro x (add a b))
< (macro x (add a b))
** </test> **********************************************************************/

static void cf_macro(void)
{
    cons(DST, main_consts.macro, ARGS);
}

/* <doc> **********************************************************************
## setq
### Type
nsubr
### Form
(setq _sym_ _sexpr_ ...)
### Description
Evaluates _sexpr_, and binds resulting value to symbol _sym_ in the current
environment.
There may be any number of _sym_s and _sexprs_.
If the last _sym_ does not have an assoicated _sexpr_, it is bound to nil.
### Return value
Last _sexpr_ evaluated, or nil.
### Exceptions
system.bind-non-symbol
: if _sym_ is not a symbol
### See also
set
### Examples
> -> (setq foo (mul 7 6))
> 42
> -> foo
> 42
> -> (setq x 13 y (add x 1) z)
> nil
> -> x
> 13
> -> y
> 14
> -> z
> nil
** </doc> **********************************************************************/

/* <test> **********************************************************************
> ;; Test setq
> (setq foo 13)
< 13
> foo
< 13
> (setq bar (mul 7 6))
< 42
> bar
< 42
> (setq foo (mul 6 7) xxx 13 aaa)
< nil
> foo
< 42
> xxx
< 13
> aaa
< nil
> (try xyz
>      (lambda (e) (assert (and (equal (car e) "system.not-bound")
>                               (equal (cadr e) 'xyz)
>                           )
>                   )
>       )
>      (assert nil)
> )
< t
** </test> **********************************************************************/

static void cf_setq(void)
{
    sx_t *work = eval_alloc(1);

    sx_t args;
    for (args = ARGS; args != 0; ) {
        sx_t sym = car(args);
        args = cdr(args);
        if (args == 0) {
            sx_assign_nil(&work[-1]);
        } else {
            sx_eval(&work[-1], car(args));
            args = cdr(args);
        }
        env_bind(sym, work[-1]);
    }
    
    sx_move(DST, &work[-1]);
}


static void cf_setq_up(void)
{
    sx_t d = env_up_except(ENVFP)->dict;

    sx_t *work = eval_alloc(1);

    sx_t args;
    for (args = ARGS; args != 0; ) {
        sx_t sym = car(args);
        args = cdr(args);
        if (args == 0) {
            sx_assign_nil(&work[-1]);
        } else {
            sx_eval(&work[-1], car(args));
            args = cdr(args);
        }
        env_bind_dict(d, sym, work[-1]);
    }
    
    sx_move(DST, &work[-1]);
}

/* <doc> **********************************************************************
## set
### Type
subr
### Form
(set _sym_ _sexpr_ ...)
### Description
Binds _sexpr_ to symbol _sym_ in the current environment.
There may be any number of _sym_s and _sexpr_s.
If the last _sym_ does not have an assoicated _sexpr_, it is bound to nil.
### Return value
Last _sexpr_, or nil.
### Exceptions
system.bind-non-symbol
: if _sym_ is not a symbol
### See also
setq
### Examples
> -> (set 'foo (mul 7 6))
> 42
> -> foo
> 42
> -> (set 'x 13 'y (add x 1) 'z)
> nil
> -> x
> 13
> -> y
> 14
> -> z
> nil
** </doc> **********************************************************************/

/* <test> **********************************************************************
> ;; Test set
> (set 'foo 13)
< 13
> foo
< 13
> (set 'bar (mul 7 6))
< 42
> bar
< 42
> (set 'foo (mul 6 7) 'xxx 13 'aaa)
< nil
> foo
< 42
> xxx
< 13
> aaa
< nil
> (try xyz
>      (lambda (e) (assert (and (equal (car e) "system.not-bound")
>                               (equal (cadr e) 'xyz)
>                           )
>                   )
>       )
>      (assert nil)
> )
< t
** </test> **********************************************************************/

static void cf_set(void)
{
    sx_t args, val = 0;
    for (args = ARGS; args != 0; ) {
        sx_t sym = car(args);
        args = cdr(args);
        if (args == 0) {
            val = 0;
        } else {
            val = car(args);
            args = cdr(args);
        }
        env_bind(sym, val);
    }
    
    sx_assign(DST, val);
}

/* <doc> **********************************************************************
## def
### Type
nsubr
### Form
(def _sym_ _sexpr_)
### Description
Binds unevaluated _sexpr_ to symbol _sym_
### Return value
Unevaluated second argument
### Exceptions
system.bind-non-symbol
: if _sym_ is not a symbol
### See also
### Examples
> -> (def test (lambda (x) (add x 1)))  
> (lambda (x) (add x 1))
** </doc> **********************************************************************/

/* <test> **********************************************************************
> (def a b)
< b
> a
< b
> (def test (lambda (x) (add x 1)))  
< (lambda (x) (add x 1))
> test
< (lambda (x) (add x 1))
** </test> **********************************************************************/

static void cf_def(void)
{
    sx_t val = 0,  args;
    for (args = ARGS; args != 0; ) {
        sx_t sym = car(args);
        args = cdr(args);
        if (args == 0) {
            val = 0;
        } else {
            val = car(args);
            args = cdr(args);
        }
        env_bind(sym, val);
    }
    
    sx_assign(DST, val);
}

/* <doc> **********************************************************************
## unsetq
### Type
nsubr
### Form
(unsetq _sym_ _sym_ ...)
### Description
### Return value
### Exceptions
system.bind-non-symbol
: if _sym_ is not a symbol
### See also
### Examples
> -> (def test (lambda (x) (add x 1)))  
> (lambda (x) (add x 1))
** </doc> **********************************************************************/

/* <test> **********************************************************************
> (def a b c d)
< d
> a
< b
> c
< d
> (unsetq a c)
< nil
> (try a
>      (lambda (e) (assert (and (equal (car e) "system.not-bound")
>                               (equal (cadr e) 'a)
>                               )
>                          )
>                  )
>      (assert nil)
> )
< t
> (try c
>      (lambda (e) (assert (and (equal (car e) "system.not-bound")
>                               (equal (cadr e) 'c)
>                               )
>                          )
>                  )
>      (assert nil)
> )
< t
** </test> **********************************************************************/

static void cf_unsetq(void)
{
    sx_t args;
    for (args = ARGS; args != 0; args = cdr(args)) {
        env_unbind(car(args));
    }
    
    sx_assign_nil(DST);
}


static void cf_boundp(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS), result;
    bool_new(DST, _env_find_from(0, &result, x, ENVFP));
}


static void cf_env_tolist(void)
{
    cf_argc_chk(1);
    sx_t *dst = DST;
    sx_assign_nil(dst);
    struct frame_env *e;
    for (e = ENVFP; e != 0; e = e->up) {
        list_append(&dst, e->dict);
    }
}


static void cf_obj_eval(void)
{
    cf_argc_chk(1);

    sx_assign(DST, car(ARGS));

#if 0
    if (debug_tracef) {
        debug_indent();
        fputs("--- ", stderr);
        sx_print(stderr, *DST); 
        fputs("\n", stderr);
    }
#endif
}


static void _cf_repr_clname(void)
{
    struct sx_strval *s = sx_inst_of(car(ARGS))->u.classval->class->name->u.strval;
    unsigned bufsize = 1 + s->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions possible */
    snprintf(buf, bufsize, "<%s>", s->data);
    str_newm(DST, bufsize, buf);
}


static void cf_obj_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (x == 0) {
        sx_assign(DST, main_consts.nil);

        return;
    }

    if (sx_type(x) != SX_TYPE_OBJ) {
        _cf_repr_clname();

        return;
    }
    
    struct sx_objval *obj = x->u.objval;
            
    sx_t *work = eval_alloc(1);
    
    struct sx_strval *s1 = obj->inst_of->u.classval->class->name->u.strval;
    struct sx_strval *s2 = sx_repr(&work[-1], obj->dict);
    unsigned bufsize = 1 + s1->size - 1 + 2 + s2->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions possible */
    snprintf(buf, bufsize, "<%s: %s>", s1->data, s2->data);
    str_newm(DST, bufsize, buf);
}


static void cf_obj_tolist(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (x != 0)  except_bad_arg(x);
    sx_assign(DST, x);
}


void _cf_obj_copy(sx_t (*func)(sx_t *, sx_t))
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_OBJ) {
        sx_assign(DST, x);

        return;
    }
        
    sx_t *work = eval_alloc(1);
        
    obj_new_init(&work[-1], x->u.objval->inst_of);
    (*func)(&work[-1]->u.objval->dict, x->u.objval->dict);
    
    sx_move(DST, &work[-1]);
}


static void cf_obj_copy(void)
{
    _cf_obj_copy(set_copy_unsafe);
}


static void cf_obj_copydeep(void)
{
    _cf_obj_copy(set_copydeep_unsafe);
}

/*
<doc>
## Obj::foreach
### Type
subr
### Form
(foreach _obj_ _callable_)
### Description
The first argument is converted to a list L, by calling its tolist method, and
then, for each item I in L, _callable_ is called, and passed I as an argument.
The function 'break' may be used in the _callable_ to exit the iteration loop prematurely.
The function 'continue' may be used in the _callable_ to skip to the next iteration.
### Return value
nil
### Exceptions
system.break-no-while-or-foreach
system.continue-no-while-or-foreach
### See also
Closure::foreach
### Examples
-> (foreach (iota 10) (lambda (x) (print x)))
0123456789nil
-> (foreach (iota 10) (lambda (x) (print x) (if (gt x 5) (break))))
0123456nil
</doc>
*/

/*
<test>
> ;; Test foreach
> (foreach (lambda (x) (print x)) (iota 10))
< 0123456789nil
> (foreach (lambda (x) (print x) (if (gt x 5) (break))) (iota 10))
< 0123456nil
</test>
*/

static void cf_foreach(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    sx_t y = cadr(ARGS);
    
    sx_t *work = eval_alloc(4);

    method_find_except(&work[-2], x, main_consts.calln);
    sx_togenerator(&work[-1], y);
    
    frame_while_push();
    FRAME_LONGJMP_SETJMP(WHILEFP);
    switch (WHILEFP->jc) {
    case FRAME_LONGJMP_JC_NONE:
    case FRAME_LONGJMP_JC_WHILE_CONTINUE:
        break;

    case FRAME_LONGJMP_JC_WHILE_BREAK:
        goto done;

    default:
        assert(0);
    }

    for (;;) {
        sx_next(&work[-3], work[-1]);
        if (work[-3] == 0)  break;

        cons(&work[-3], x, cons(&work[-3], car(work[-3]), 0));
        method_call_run(&work[-4], work[-2], 2, work[-3], ARGS_EVAL_SUPPRESS_ALL);
    }

 done:
    sx_assign_nil(DST);
}


static void cf_int_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(ARGS);
    intval_t i;
    switch (sx_type(x)) {
    case SX_TYPE_INT:
        sx_assign(DST, x);
        return;

    case SX_TYPE_FLOAT:
        i = (intval_t) x->u.floatval;
        break;

    case SX_TYPE_COMPLEX:
        if (x->u.complexval->im != 0.0)  except_bad_arg(x);
        i = (intval_t) x->u.complexval->re;
        break;

    case SX_TYPE_STR:
    case SX_TYPE_SYM:
        {
            struct sx_strval *s = x->u.strval;
            unsigned size = s->size - 1;
            const char *data = s->data;
            trim(&size, &data);
            char buf[size + 1];
            memcpy(buf, data, size);
            buf[size] = 0;
            if (!int_from_str(&i, size + 1, data))  except_bad_arg(x);
        }
        break;

    default:
        except_bad_arg(x);
    }

    int_new(DST, i);
}


static void cf_int_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", x->u.intval);
    str_newc1(DST, buf);
}


static void cf_int_cmp(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    intval_t i = x->u.intval;
    
    int cmp = 0;
    sx_t y = cadr(ARGS);
    switch (sx_type(y)) {
    case SX_TYPE_INT:
        {
            intval_t j = y->u.intval;
            if (i < j)       cmp = -1;
            else if (i > j)  cmp = 1;
            else             cmp = 0;
        }
        break;

    case SX_TYPE_FLOAT:
        {
            floatval_t f = (floatval_t) i, g = y->u.floatval;
            if (f < g)       cmp = -1;
            else if (f > g)  cmp = 1;
            else             cmp = 0;
        }
        break;

    default:
        except_bad_arg(y);
    }

    int_new(DST, cmp);
}


static void cf_int_equal(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    intval_t i = x->u.intval;

    bool f = false;
    sx_t y = cadr(ARGS);
    switch (sx_type(y)) {
    case SX_TYPE_INT:
        f = (y->u.intval == i);
        break;
    case SX_TYPE_FLOAT:
        f = (y->u.floatval == (floatval_t) i);
        break;
    default: ;
    }

    bool_new(DST, f);
}


static void cf_float_equal(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FLOAT)  except_bad_arg(x);
    floatval_t f = x->u.floatval;

    bool fl = false;
    sx_t y = cadr(ARGS);
    switch (sx_type(y)) {
    case SX_TYPE_INT:
        fl = ((floatval_t) y->u.intval == f);
        break;
    case SX_TYPE_FLOAT:
        fl = (y->u.floatval == f);
        break;
    default: ;
    }

    bool_new(DST, fl);
}


static void cf_int_hash(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    
    sx_assign(DST, x);
}

static void cf_float_repr(void);

static void cf_float_hash(void)
{
    cf_float_repr();
    int_new(DST, str_hash((*DST)->u.strval));
}


void _cf_add(struct sx_numeric *result)
{
    sx_t args;
    for (args = cdr(ARGS); args != 0; args = cdr(args)) {
        sx_t x = car(args);
        struct sx_numeric a[1];
        if (!numeric_from_sx(a, x))  except_bad_arg2(STR_CONST(type_mismatch), x);
        unsigned t = numeric_type_max(result, a);
        numeric_promote(result, t);
        numeric_promote(a, t);
        switch (t) {
        case SX_TYPE_INT:
            result->u.intval += a->u.intval;
            break;
        case SX_TYPE_FLOAT:
            result->u.floatval += a->u.floatval;
            break;
        case SX_TYPE_COMPLEX:
            result->u.complexval->re += a->u.complexval->re;
            result->u.complexval->im += a->u.complexval->im;
            break;
        default:
            assert(0);
        }
    }

    numeric_new(DST, result);
}


static void cf_int_abs(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    intval_t i = x->u.intval;
    if (i < 0) {
        int_new(DST, -i);

        return;
    }

    sx_assign(DST, x);
}


static void cf_int_re(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    sx_assign(DST, x);
}


static void cf_int_im(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    int_new(DST, 0);
}


static void cf_int_add(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_int(result, x->u.intval);
    _cf_add(result);
}


void _cf_sub(struct sx_numeric *result)
{
    sx_t y = cadr(ARGS);
    struct sx_numeric a[1];
    if (!numeric_from_sx(a, y))  except_bad_arg2(STR_CONST(type_mismatch), y);
    unsigned t = numeric_type_max(result, a);
    numeric_promote(result, t);
    numeric_promote(a, t);
    switch (t) {
    case SX_TYPE_INT:
        result->u.intval -= a->u.intval;
        break;
    case SX_TYPE_FLOAT:
        result->u.floatval -= a->u.floatval;
        break;
    case SX_TYPE_COMPLEX:
        result->u.complexval->re -= a->u.complexval->re;
        result->u.complexval->im -= a->u.complexval->im;
        break;
    default:
        assert(0);
    }

    numeric_new(DST, result);
}


static void cf_int_sub(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_int(result, x->u.intval);
    _cf_sub(result);
}


void _cf_mul(struct sx_numeric *result)
{
    sx_t args;
    for (args = cdr(ARGS); args != 0; args = cdr(args)) {
        sx_t x = car(args);
        struct sx_numeric a[1];
        if (!numeric_from_sx(a, x))  except_bad_arg2(STR_CONST(type_mismatch), x);
        unsigned t = numeric_type_max(result, a);
        numeric_promote(result, t);
        numeric_promote(a, t);
        switch (t) {
        case SX_TYPE_INT:
            result->u.intval *= a->u.intval;
            break;
        case SX_TYPE_FLOAT:
            result->u.floatval *= a->u.floatval;
            break;
        case SX_TYPE_COMPLEX:
            {
                floatval_t re = result->u.complexval->re * a->u.complexval->re
                    - result->u.complexval->im * a->u.complexval->im;
                floatval_t im = result->u.complexval->re * a->u.complexval->im
                    + result->u.complexval->im * a->u.complexval->re;
                numeric_from_complex(result, re, im);
            }
            break;
        default:
            assert(0);
        }
    }

    numeric_new(DST, result);
}


static void cf_int_mul(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_int(result, x->u.intval);
    _cf_mul(result);
}


void _cf_div(struct sx_numeric *result)
{
    sx_t y = cadr(ARGS);
    struct sx_numeric a[1];
    if (!numeric_from_sx(a, y))  except_bad_arg2(STR_CONST(type_mismatch), y);
    unsigned t = numeric_type_max(result, a);
    numeric_promote(result, t);
    numeric_promote(a, t);
    switch (t) {
    case SX_TYPE_INT:
        result->u.intval /= a->u.intval;
        break;
    case SX_TYPE_FLOAT:
        result->u.floatval /= a->u.floatval;
        break;
    case SX_TYPE_COMPLEX:
        {
            float d = a->u.complexval->re * a->u.complexval->re + a->u.complexval->im * a->u.complexval->im;
            float re = (result->u.complexval->re * a->u.complexval->re + result->u.complexval->im * a->u.complexval->im) / d;
            float im = (result->u.complexval->im * a->u.complexval->re - result->u.complexval->re * a->u.complexval->im) / d;
            result->u.complexval->re = re;
            result->u.complexval->im = im;
        }
        break;
    default:
        assert(0);
    }

    numeric_new(DST, result);
}


static void cf_int_div(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_int(result, x->u.intval);
    _cf_div(result);
}


static void cf_float_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(ARGS);
    floatval_t val;
    switch (sx_type(x)) {
    case SX_TYPE_FLOAT:
        sx_assign(DST, x);
        return;

    case SX_TYPE_INT:
        val = (floatval_t) x->u.intval;
        break;

    case SX_TYPE_COMPLEX:
        if (x->u.complexval->im != 0.0)  except_bad_arg(x);
        val = x->u.complexval->re;
        break;

    case SX_TYPE_STR:
    case SX_TYPE_SYM:
        {
            struct sx_strval *s = x->u.strval;
            unsigned size = s->size - 1;
            const char *data = s->data;
            trim(&size, &data);
            char buf[size + 1];
            memcpy(buf, data, size);
            buf[size] = 0;
            if (!float_from_str(&val, size + 1, data))  except_bad_arg(x);
        }
        break;

    default:
        except_bad_arg(x);
    }

    float_new(DST, val);
}


static void cf_float_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FLOAT)  except_bad_arg(x);

    char buf[64];
    snprintf(buf, sizeof(buf), "%Lg", x->u.floatval);
    str_newc1(DST, buf);
}


static void cf_float_add(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FLOAT)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_float(result, x->u.floatval);
    _cf_add(result);
}


static void cf_float_sub(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FLOAT)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_float(result, x->u.floatval);
    _cf_sub(result);
}


static void cf_float_mul(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FLOAT)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_float(result, x->u.floatval);
    _cf_mul(result);
}


static void cf_complex_new(void)
{
    floatval_t re = 0.0, im = 0.0;
    switch (ARGC) {
    case 2:
        {
            sx_t x = cadr(ARGS);
            switch (sx_type(x)) {
            case SX_TYPE_INT:
                re = (floatval_t) x->u.intval;
                break;

            case SX_TYPE_FLOAT:
                re = x->u.floatval;
                break;

            case SX_TYPE_COMPLEX:
                sx_assign(DST, x);
                return;
                
            case SX_TYPE_STR:
            case SX_TYPE_SYM:
                {
                    struct sx_strval *s = x->u.strval;
                    unsigned size = s->size - 1;
                    const char *data = s->data;
                    trim(&size, &data);
                    char buf[size + 1];
                    memcpy(buf, data, size);
                    buf[size] = 0;
                    if (!complex_from_str(&re, &im, size + 1, data))  except_bad_arg(x);
                }
                break;
                
            default:
                except_bad_arg(x);
            }
        }
        break;
        
    case 3:
        {
            sx_t x = cadr(ARGS), y = car(cdr(cdr(ARGS)));
            switch (sx_type(x)) {
            case SX_TYPE_FLOAT:
                re = x->u.floatval;
                break;

            case SX_TYPE_INT:
                re = (floatval_t) x->u.intval;
                break;

            default:
                except_bad_arg(x);
            }
            switch (sx_type(y)) {
            case SX_TYPE_FLOAT:
                im = y->u.floatval;
                break;

            case SX_TYPE_INT:
                im = (floatval_t) y->u.intval;
                break;

            default:
                except_bad_arg(y);
            }
        }
        break;
        
    default:
        except_num_args_range(2, 3);
    }

    complex_new(DST, re, im);
}


static void cf_complex_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_COMPLEX)  except_bad_arg(x);

    char buf[128];
    if (x->u.complexval->im == 0.0) {
        snprintf(buf, sizeof(buf), "%Lg", x->u.complexval->re);
    } else if (x->u.complexval->re == 0.0) {
        snprintf(buf, sizeof(buf), "%Lgi", x->u.complexval->im);
    } else {
        snprintf(buf, sizeof(buf), "%Lg%s%Lgi",
                 x->u.complexval->re,
                 x->u.complexval->im > 0.0 ? "+" : "",
                 x->u.complexval->im
                 );
    }
    str_newc1(DST, buf);
}


static void cf_complex_hash(void)
{
    cf_complex_repr();
    int_new(DST, str_hash((*DST)->u.strval));
}


static void cf_complex_add(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_COMPLEX)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_complex(result, x->u.complexval->re, x->u.complexval->im);
    _cf_add(result);
}


static void cf_complex_sub(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_COMPLEX)  except_bad_arg(x);
    
    struct sx_numeric result[1];
    numeric_from_complex(result, x->u.complexval->re, x->u.complexval->im);
    _cf_sub(result);
}

/* <doc> **********************************************************************
## String::at
### Type
subr
### Form
(at _in_ _ofs_)
### Description
Return string which is the character at offset _ofs_, which must be an integer,
in string given by _in_.  A negative value for _ofs_ is taken as an offset from
the end of _in_.
### Return value
See above
### Exceptions
- system.index-range
### See also
ate
### Examples
> -> (at "The rain in Spain" 1)
> h
** </doc> **********************************************************************/

/* <doc> **********************************************************************
## String::ate
### Type
subr
### Form
(ate _in_ _which_)
### Description
This function is the same as String::at -- an exception for no member has no
meaning for a string.
### Return value
See above
### Exceptions
- system.index-range
### See also
at
** </doc> **********************************************************************/

static void cf_str_cmp(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (!sx_is_str(x))  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (!sx_is_str(y))  except_bad_arg(y);

    int cmp = strcmp(x->u.strval->data, y->u.strval->data);
    if (cmp < 0)       cmp = -1;
    else if (cmp > 0)  cmp = 1;

    int_new(DST, cmp);
}


static void cf_str_tonumber(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_str(x))  except_bad_arg(x);
    struct sx_strval *s = x->u.strval;
    unsigned size = s->size - 1;
    const char *data = s->data;
    trim(&size, &data);
    char buf[size + 1];
    memcpy(buf, data, size);
    buf[size] = 0;
    ++size;
    struct sx_numeric val[1];
    do {
        if (int_from_str(&val->u.intval, size, buf)) {
            val->type = SX_TYPE_INT;
            break;
        }
        if (float_from_str(&val->u.floatval, size, buf)) {
            val->type = SX_TYPE_FLOAT;
            break;
        }
        if (complex_from_str(&val->u.complexval->re, &val->u.complexval->im, size, buf)) {
            val->type = SX_TYPE_COMPLEX;
            break;
        }
        
        except_bad_arg(x);
    } while (0);

    numeric_new(DST, val);
}


static void cf_str_new(void)
{
    cf_argc_chk(2);
    sx_tostring(DST, cadr(ARGS));
}


static inline unsigned decr_if_nz(unsigned x)
{
    return (x == 0 ? x : x - 1);
}


static void cf_str_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);

    struct sx_strval *s = x->u.strval;
    unsigned bufsize = 1 + s->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions possible */
    snprintf(buf, bufsize, "\"%s\"", s->data);
    str_newm(DST, bufsize, buf);
}


static void cf_str_hash(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_str(x))  except_bad_arg(x);
    int_new(DST, str_hash(x->u.strval));
}


static void cf_str_equal(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    bool_new(DST, sx_type(y) == SX_TYPE_STR && str_equal(x->u.strval, y->u.strval));
}


static void cf_str_tostring(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);

    sx_assign(DST, x);
}


sx_t str_join(sx_t *dst,
              unsigned size,
              unsigned ldr_size, const char *ldr,
              unsigned sep_size, const char *sep,
              unsigned trlr_size, const char *trlr,
              sx_t li
              )
{
    ldr_size = decr_if_nz(ldr_size);
    sep_size = decr_if_nz(sep_size);
    trlr_size = decr_if_nz(trlr_size);

    char *buf = mem_alloc(size); /* Safe, no exception can occur */
    char *p = buf;

    memcpy(p, ldr, ldr_size);
    p += ldr_size;

    bool f = false;
    for (; li != 0; li = cdr(li), f = true) {
        if (f) {
            memcpy(p, sep, sep_size);
            p += sep_size;
        }

        struct sx_strval *s = car(li)->u.strval;
        unsigned n = decr_if_nz(s->size);
        memcpy(p, s->data, n);
        p += n;
    }

    memcpy(p, trlr, trlr_size);
    p += trlr_size;

    *p = 0;
    
    return (str_newm(dst, size, buf));
}


static void cf_str_join(void)
{
    cf_argc_chk_min(1);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);
    struct sx_strval *sep = x->u.strval;
    unsigned sep_size = decr_if_nz(sep->size);
    args = cdr(args);

    unsigned size = 1;
    bool f = false;
    sx_t p;
    for (p = args; p != 0; p = cdr(p), f = true) {
        if (f)  size += sep_size;
        
        sx_t y = car(p);
        if (!sx_is_str(y))  except_bad_arg(y);
        size += decr_if_nz(y->u.strval->size);
    }

    str_join(DST, size, 0, 0, sep->size, sep->data, 0, 0, args);
}


static void cf_int_octal(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    char buf[32];
    snprintf(buf, sizeof(buf), "%llo", x->u.intval);
    str_newc1(DST, buf);
}


static void cf_int_hex(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    char buf[32];
    snprintf(buf, sizeof(buf), "%llx", x->u.intval);
    str_newc1(DST, buf);
}


static void cf_sym_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(ARGS);

    sx_t *work = eval_alloc(1);

    sx_tostring(&work[-1], x);
    struct sx_strval *s = work[-1]->u.strval;
    sym_newc(DST, s->size, s->data);
}


static void cf_sym_eval(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);

#if 0
    if (debug_tracef) {
        debug_indent();
        fputs(">>> ", stderr);
        sx_print(stderr, x);
        fputs("\n", stderr);
    }
#endif
    
    if (sx_type(x) != SX_TYPE_SYM)  except_bad_arg(x);
    sx_assign(DST, env_find(x));

#if 0
    if (debug_tracef) {
        debug_indent();
        fputs("<<< ", stderr);
        sx_print(stderr, *DST);
        fputs("\n", stderr);
    }
#endif
}


static void cf_sym_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_SYM)  except_bad_arg(x);

    struct sx_strval *s = x->u.strval;
    str_newc(DST, s->size, s->data);
}


static void cf_sym_equal(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS), y = cadr(ARGS);
    if (sx_type(x) != SX_TYPE_SYM)  except_bad_arg(x);

    bool_new(DST, x == y);
}


static void cf_subr_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    switch (sx_type(x)) {
    case SX_TYPE_SUBR:
    case SX_TYPE_NSUBR:
        break;
    default:
        except_bad_arg(x);
    }

    struct sx_strval *s1 = sx_inst_of_class_name(x);
    struct sx_strval *s2 = x->u.codefuncval->name->u.strval;
    unsigned bufsize = 1 + s1->size - 1 + 2 + s2->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions possible */
    snprintf(buf, bufsize, "<%s: %s>", s1->data, s2->data);
    str_newm(DST, bufsize, buf);
}


static void subr_call(int args_eval_suppress_cnt)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_SUBR)  except_bad_arg(x);

    sx_t *work = eval_alloc(2);

    sx_t args, *p;
    for (p = &work[-1], args = cdr(ARGS); args != 0; args = cdr(args)) {
        if (args_eval_suppress_cnt != 0) {
            list_append(&p, car(args));

            if (args_eval_suppress_cnt > 0)  --args_eval_suppress_cnt;

            continue;
        }
        
        sx_eval(&work[-2], car(args));
        list_append(&p, work[-2]);
    }

    eval_pop(1);
    
    frame_func_push(DST, x, ARGC - 1, work[-1]);
    
    (*x->u.codefuncval->func)();
}


static void cf_subr_call(void)  /* Nsubr */
{
    subr_call(0);
}


static void cf_subr_call1(void) /* Nsubr */
{
    subr_call(1);
}


static void cf_subr_calln(void) /* Nsubr */
{
    subr_call(-1);
}


static void cf_nsubr_call(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_NSUBR)  except_bad_arg(x);

    frame_func_push(DST, x, ARGC - 1, cdr(ARGS));
    
    (*x->u.codefuncval->func)();
}


static void args_lambda(sx_t env_dict, sx_t form, unsigned argc, sx_t args, int args_eval_suppress_cnt)
{
    unsigned argc_expected, argc_default;
    sx_t form_args = cadr(form), x, y;
    for (argc_expected = argc_default = 0, x = form_args; x != 0; x = cdr(x)) {
        if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(form);
        y = car(x);
        if (sx_type(y) == SX_TYPE_SYM) {
            if (argc_default > 0)  except_bad_arg(form);

            ++argc_expected;

            continue;
        }
        if (sx_type(y) == SX_TYPE_DPTR && sx_type(car(y)) == SX_TYPE_SYM) {
            ++argc_default;
            
            continue;
        }

        except_bad_arg(form);
    }

    if (argc < argc_expected || argc > (argc_expected + argc_default)) {
        if (argc_default == 0)  except_num_args(argc_expected);
        except_num_args_range(argc_expected, argc_expected + argc_default);
    }    

    sx_t *work = eval_alloc(2);              /* Reclaimed in caller */

    sx_t *p = &work[-1];
    for (; args != 0; args = cdr(args)) {
        if (args_eval_suppress_cnt != 0) {
            list_append(&p, car(args));
            if (args_eval_suppress_cnt > 0)  --args_eval_suppress_cnt;

            continue;
        }
        
        sx_eval(&work[-2], car(args));
        list_append(&p, work[-2]);
    }

    /* work[-1] is evalled arg list */

    FUNCFP->args = work[-1];    /* Adjust passed args to evalled args */
    
    eval_pop(1);
    
    for (y = work[-1]; form_args != 0; form_args = cdr(form_args)) {
        sx_t x = car(form_args);
        if (sx_type(x) == SX_TYPE_SYM) {
            DEBUG_ASSERT(y != 0);
            env_bind_dict(env_dict, x, car(y));
            y = cdr(y);

            continue;
        }

        DEBUG_ASSERT(sx_type(x) == SX_TYPE_DPTR);
        if (y == 0) {
            env_bind_dict(env_dict, car(x), cdr(x));

            continue;
        }

        env_bind_dict(env_dict, car(x), car(y));
        y = cdr(y);
    }
}


static void args_nlambda(sx_t env_dict, sx_t form, sx_t args)
{
    sx_t form_arg = cadr(form);
    if (sx_type(form_arg) != SX_TYPE_SYM)  except_bad_arg(form);
    
    env_bind_dict(env_dict, form_arg, args);
}


static void _dptr_call(sx_t *dst, sx_t form, unsigned argc, sx_t args, int args_eval_suppress_cnt)
{
    if (!list_len_chk_min(form, 2))  except_bad_arg(form);

    struct vm_state_save vmsave[1];
    vm_state_save(vmsave);
    
    sx_t *work = eval_alloc(1);
    dict_new(&work[-1], 32);

    frame_func_push(dst, form, argc, args);
    
    sx_t y = car(form);
    if (y == main_consts.lambda) {
        args_lambda(work[-1], form, argc, args, args_eval_suppress_cnt);
    } else if (y == main_consts.nlambda || y == main_consts.macro) {
        args_nlambda(work[-1], form, args);
    } else  except_bad_arg(form);

    frame_env_push(work[-1]);

    for (form = cdr(cdr(form)); form != 0; form = cdr(form)) {
        sx_eval(dst, car(form));
    }

    vm_state_restore(vmsave);

    if (y == main_consts.macro)  sx_eval(dst, *dst);
}


static void dptr_call(int args_eval_suppress_cnt)
{
    cf_argc_chk_min(1);
    _dptr_call(DST, car(ARGS), ARGC - 1, cdr(ARGS), args_eval_suppress_cnt);
}


static void cf_dptr_call(void)
{
    dptr_call(0);
}


static void cf_dptr_call1(void)
{
    dptr_call(1);
}


static void cf_dptr_calln(void)
{
    dptr_call(-1);
}


static void cf_method_call(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_METHOD)  except_bad_arg(x);
    struct sx_methodval *m = x->u.methodval;

    frame_class_push(m->class);

    sx_t *work = eval_alloc(1);

    cons(&work[-1], m->func, cdr(ARGS));
    method_call_apply(DST, main_consts.call, ARGC, work[-1], 0);
}


static void cf_method_call1(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_METHOD)  except_bad_arg(x);
    struct sx_methodval *m = x->u.methodval;

    frame_class_push(m->class);

    sx_t *work = eval_alloc(1);

    cons(&work[-1], m->func, cdr(ARGS));
    method_call_apply(DST, main_consts.call1, ARGC, work[-1], 1);
}


static void cf_method_calln(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_METHOD)  except_bad_arg(x);
    struct sx_methodval *m = x->u.methodval;

    frame_class_push(m->class);

    sx_t *work = eval_alloc(1);

    cons(&work[-1], m->func, cdr(ARGS));
    method_call_apply(DST, main_consts.calln, ARGC, work[-1], ARGS_EVAL_SUPPRESS_ALL);
}


static void cf_closure_call(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSURE)  except_bad_arg(x);
    struct sx_closureval *cl = x->u.closureval;

    frame_env_push(cl->dict);

    sx_t *work = eval_alloc(1);

    cons(&work[-1], cl->expr, cdr(ARGS));
    method_call_apply(DST, main_consts.call, ARGC, work[-1], 0);
}


static void cf_closure_call1(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSURE)  except_bad_arg(x);
    struct sx_closureval *cl = x->u.closureval;

    frame_env_push(cl->dict);

    sx_t *work = eval_alloc(1);

    cons(&work[-1], cl->expr, cdr(ARGS));
    method_call_apply(DST, main_consts.call1, ARGC, work[-1], 0);
}


static void cf_closure_calln(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSURE)  except_bad_arg(x);
    struct sx_closureval *cl = x->u.closureval;

    frame_env_push(cl->dict);

    sx_t *work = eval_alloc(1);

    cons(&work[-1], cl->expr, cdr(ARGS));
    method_call_apply(DST, main_consts.calln, ARGC, work[-1], 0);
}


static void cf_exit(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    myexit(x->u.intval);
}


void except_file_not_open(sx_t sx)
{
    except_raise1();
    
    except_newl(STR_CONST("system.file-not-open"), 1, sx);

    frame_except_raise();
}


static void cf_file_read(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);

    frame_input_push(x, NON_INTERACTIVE);

    sx_read(DST);
}


void sx_readb(sx_t *dst, FILE *fp, bool limit_valid, unsigned limit)
{
    unsigned bufsize = 4096, readsize = bufsize, size = 0;
    unsigned char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
    
    for (;;) {
        if (limit_valid && readsize > (limit - size)) {
            readsize = limit - size;
            if (readsize == 0)  break;
        }
        int n = fread(buf + size, 1, readsize, fp);
        size += n;
        if (n < readsize)  break;
        readsize = bufsize;
        unsigned newsize = bufsize << 1;
        assert(newsize != 0);
        
        bufsize = newsize;
        buf = mem_realloc(buf, bufsize, newsize);
    }

    barray_newm(dst, size, mem_realloc(buf, bufsize, size));    
}


static void cf_file_readb(void)
{
    cf_argc_chk_range(1, 2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);
    
    unsigned limit = 0;
    bool limit_valid = false;
    if (ARGC == 2) {
        sx_t y = cadr(ARGS);
        if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
        intval_t yy = y->u.intval;
        if (yy < 0)  except_bad_arg(y);
        limit = yy;
        limit_valid = true;
    }

    sx_readb(DST, x->u.fileval->fp, limit_valid, limit);
}


static void except_file_open_failed(sx_t filename, sx_t mode)
{
    except_raise1();

    sx_t *work = eval_alloc(1);

    str_newc1(&work[-1], strerror(errno));
    except_newl(STR_CONST("system.file-open-failed"), 3, filename, mode, work[-1]);

    frame_except_raise();
}


static void cf_str_read(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);
    struct sx_strval *s = x->u.strval;

    sx_t *work = eval_alloc(2);

    str_newb(&work[-1], STR_CONST("<String>"));
    static const char mode[] = "r";
    str_newb(&work[-2], STR_CONST(mode));
    FILE *fp = fmemopen((char *) s->data, s->size - 1, mode);
    if (fp == 0)  except_file_open_failed(work[-1], work[-2]);

    file_new(&work[-1], work[-1], work[-2], fp);

    eval_pop(1);

    frame_input_push(work[-1], NON_INTERACTIVE);

    sx_read(DST);
}


static void cf_str_split(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS), y = cadr(ARGS);
    if (sx_is_str(x)) {
        struct sx_strval *s1 = x->u.strval;
        if (!sx_is_str(y))  except_bad_arg(y);
        struct sx_strval *s2 = y->u.strval;
        const char *s = s2->data;
        unsigned n2 = s2->size - 1, n1 = s1->size - 1;

        sx_t *work = eval_alloc(1);

        sx_t *dst = DST;
        sx_assign_nil(dst);
        for (;;) {
            char *q = strstr(s, s1->data);
            unsigned k = (q == 0) ? n2 : q - s;
            list_append(&dst, str_newc(&work[-1], k + 1, s));
            if (q == 0)  break;
            s = q + n1;
            n2 -= k + n1;
        }

        return;
    }
    
    except_bad_arg(x);
}


void except_idx_range(sx_t idx)
{
    except_raise1();

    except_newl(STR_CONST("system.index-range"), 1, idx);

    frame_except_raise();
}


static void cf_str_index(void)
{
    cf_argc_chk_range(2, 3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (!sx_is_str(y))  except_bad_arg(y);
    intval_t ofs = 0;
    if (ARGC == 3) {
        sx_t z = cadr(args);
        if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
        ofs = z->u.intval;
        if (!slice1(&ofs, x->u.strval->size))  except_idx_range(z);
    }
    
    const char *s = x->u.strval->data;
    char *p = strstr(s + ofs, y->u.strval->data);
    if (p == 0) {
        sx_assign_nil(DST);

        return;
    }

    int_new(DST, p - s);
}


static void cf_str_rindex(void)
{
    cf_argc_chk_range(2, 3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (!sx_is_str(y))  except_bad_arg(y);
    struct sx_strval *s1 = x->u.strval, *s2 = y->u.strval;
    unsigned n = s2->size - 1;
    const char *p;
    if (ARGC == 3) {
        sx_t z = cadr(args);
        if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
        intval_t ofs = z->u.intval;
        if (!slice1(&ofs, s1->size))  except_idx_range(z);
        p = s1->data + ofs;
    } else {
        p = s1->data + (s1->size - 1) - n;                
    }
    
    for (; p >= s1->data; --p) {
        if (strncmp(p, s2->data, n) == 0) {
            int_new(DST, p - s1->data);
            
            return;
        }
    }
    
    sx_assign_nil(DST);
}

void except_index_range(sx_t ofs);
void except_index_range2(sx_t ofs, sx_t len);

static void cf_str_slice(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    intval_t ofs = y->u.intval;
    args = cdr(args);  sx_t z = car(args);
    if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
    intval_t len = z->u.intval;
    if (!slice(&ofs, &len, x->u.strval->size - 1))  except_index_range2(y, z);
    char *buf = mem_alloc(len + 1); /* Safe, no exceptions can occur */
    memcpy(buf, &x->u.strval->data[ofs], len);
    buf[len] = 0;
    str_newm(DST, len + 1, buf);
}


static void cf_str_at(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (!sx_is_str(x))  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    intval_t ofs = y->u.intval;
    if (!slice1(&ofs, x->u.strval->size - 1))  except_index_range(y);
    char *buf = mem_alloc(2);   /* Safe, no exceptions can occur */
    buf[0] = x->u.strval->data[ofs];
    buf[1] = 0;
    str_newm(DST, 2, buf);
}

sx_t list_at(sx_t li, unsigned idx);

static void cf_str_format(void)
{
    cf_argc_chk_min(1);
    int argc = ARGC;
    sx_t args = ARGS;
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);
    const char *s = x->u.strval->data;
    unsigned rem = x->u.strval->size - 1;
    --argc;  args = cdr(args);

    sx_t *work = eval_alloc(2);

    sx_t *p = &work[-1];
    unsigned size = 1;
    intval_t last_idx = -1;
    while (rem > 0) {
        char *q = index(s, '{');
        unsigned n = (q == 0) ? rem : q - s;
        /* This borrow is ok because:
           (1) the underlying string is an argument, and won't go away; and
           (2) str_join doesn't need the null terminator on its input strings
         */
        str_newb(&work[-2], n + 1, s);
        list_append(&p, work[-2]);
        size += n;
        
        if (q == 0)  break;

        char *r = index(q + 1, '}');
        if (r == 0)  except_bad_arg(x);
        unsigned bufsize = (r - (q + 1)) + 1;
        intval_t idx;
        if (bufsize == 1) {
            idx = ++last_idx;
            if (idx >= argc)  except_bad_arg(x);
        } else {
            char buf[bufsize];
            memcpy(buf, q + 1, bufsize - 1);
            buf[bufsize - 1] = 0;
            if (!(sscanf(buf, "%lld", &idx) == 1
                  && slice1(&idx, argc)
                  )
                ) {
                except_bad_arg(x);
            }
        }
        sx_tostring(&work[-2], list_at(args, idx));
        list_append(&p, work[-2]);
        size += work[-2]->u.strval->size - 1;

        n = r + 1 - s;
        rem -= n;
        s += n;
    }

    eval_pop(1);

    str_join(DST, size, 0, 0, 0, 0, 0, 0, work[-1]);
}


static void cf_str_size(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_str(x))  except_bad_arg(x);

    int_new(DST, x->u.strval->size - 1);
}


static void cf_pass(void)
{
    cf_argc_chk(1);
    sx_assign(DST, car(ARGS));
}


static void cf_eval(void)
{
    cf_argc_chk(1);
    sx_eval(DST, car(ARGS));
}


static void cf_funccall(void)   /* Subr */
{
    cf_argc_chk_min(1);

    /* Arguments are evalled, since this is a Subr
       -- suppress futher evaluation
     */
    method_call_apply(DST, main_consts.calln, ARGC, ARGS, ARGS_EVAL_SUPPRESS_ALL);
}


static void cf_apply(void)      /* Subr */
{
    cf_argc_chk(2);
    sx_t y = cadr(ARGS);
    int n = list_len(y);
    if (n < 0)  except_bad_arg(y);

    sx_t *work = eval_alloc(1);

    /* Arguments are evalled, since this is a Subr
       -- suppress futher evaluation
     */
    cons(&work[-1], car(ARGS), y);
    method_call_apply(DST, main_consts.calln, 1 + (unsigned) n, work[-1], ARGS_EVAL_SUPPRESS_ALL);
}


static void cf_method_funccall(void)   /* Subr */
{
    cf_argc_chk_min(2);

    sx_t *work = eval_alloc(1);

    /* Arguments are evalled, since this is a Subr
       -- suppress futher evaluation
     */
    method_find_except(&work[-1], cadr(ARGS), car(ARGS));
    cons(&work[-1], work[-1], cdr(ARGS));
    method_call_apply(DST, main_consts.calln, ARGC, work[-1], ARGS_EVAL_SUPPRESS_ALL);
}


static void cf_method_apply(void) /* Nsubr */
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    sx_t y = cadr(ARGS);
    int n = list_len(y);
    if (n < 0)  except_bad_arg(y);

    sx_t *work = eval_alloc(1);

    /* Arguments are evalled, since this is a Subr
       -- suppress futher evaluation
     */
    method_find_except(&work[-1], car(y), x);
    cons(&work[-1], work[-1], y);
    method_call_apply(DST, main_consts.calln, 1 + (unsigned) n, work[-1], ARGS_EVAL_SUPPRESS_ALL);
}


static void cf_call0(void)
{
    cf_argc_chk(1);

    method_call_apply(DST, main_consts.calln, 1, ARGS, ARGS_EVAL_SUPPRESS_ALL);    
}


static void cf_print(void)
{
    cf_argc_chk_range(1, 2);
    FILE *fp = stdout;
    sx_t x;
    if (ARGC == 2) {
        x = cadr(ARGS);
        if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);
        fp = x->u.fileval->fp;
    }

    x = car(ARGS);
        
    sx_print(fp, x);

    sx_assign(DST, x);
}


static void cf_file_write(void)
{    
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);

    unsigned char *data = 0;
    unsigned size = 0;
    sx_t y = cadr(ARGS);
    if (sx_is_str(y)) {
        struct sx_strval *s = y->u.strval;
        data = (unsigned char *) s->data;
        size = s->size - 1;
    } else if (sx_type(y) == SX_TYPE_BARRAY) {
        struct sx_barrayval *b = y->u.barrayval;
        data = b->data;
        size = b->size;
    } else  except_bad_arg(y);

    int_new(DST, fwrite(data, 1, size, x->u.fileval->fp));
}


static void cf_file_fflush(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)   except_bad_arg(x);

    fflush(x->u.fileval->fp);

    sx_assign(DST, x);
}


static void cf_file_new(void)
{
    cf_argc_chk(3);
    sx_t args = cdr(ARGS);
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (!sx_is_str(y))  except_bad_arg(y);
    FILE *fp = fopen(x->u.strval->data, y->u.strval->data);
    if (fp == 0)  except_file_open_failed(x, y);

    file_new(DST, x, y, fp);
}


static void cf_file_copy(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);

    file_copy(DST, x);
}


static void cf_file_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);

    struct sx_strval *clnm = sx_inst_of_class_name(x);
    struct sx_fileval *f = x->u.fileval;
    struct sx_strval *ns = f->filename->u.strval;
    struct sx_strval *ms = f->mode->u.strval;
    unsigned bufsize = 1 + clnm->size - 1 + 2 + ns->size - 1 + 1 + ms->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
    snprintf(buf, bufsize, "<%s: %s %s>", clnm->data, ns->data, ms->data);
    str_newm(DST, bufsize, buf);
}


static void cf_file_lseek(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    args = cdr(args);  sx_t z = car(args);
    if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
    int whence = z->u.intval;
    if (!(whence >= 0 && whence <= 2))  except_bad_arg(z);
    
    int_new(DST, fseek(x->u.fileval->fp, y->u.intval, whence));
}


static void cf_file_tell(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);
    
    int_new(DST, ftell(x->u.fileval->fp));
}


static void cf_file_close(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_FILE)  except_bad_arg(x);
    FILE *fp = x->u.fileval->fp;
    if (fp != 0) {
        fclose(fp);
        x->u.fileval->fp = 0;
    }

    sx_assign_nil(DST);
}


void except_no_label(sx_t label)
{
    except_raise1();
    
    except_newl(STR_CONST("system.label-not-found"), 1, label);

    frame_except_raise();
}


static void cf_prog(void)
{
    sx_t *work = eval_alloc(2), x = ARGS;
    
    frame_prog_push(&work[-1]);
    FRAME_LONGJMP_SETJMP(PROGFP);
    switch (PROGFP->jc) {
    case FRAME_LONGJMP_JC_NONE:
        break;
        
    case FRAME_LONGJMP_JC_PROG_GOTO:
        for (x = ARGS; x != 0; x = cdr(x)) {
            sx_t y = car(x);
            if (sx_type(y) == SX_TYPE_SYM && strcmp(y->u.strval->data, work[-1]->u.strval->data) == 0)  break;
        }
        if (x == 0)  except_no_label(work[-1]);
        x = cdr(x);
                
        break;
        
    case FRAME_LONGJMP_JC_PROG_RETURN:
        sx_move(DST, &work[-1]);

        return;

    default:
        assert(0);
    }
    
    for (; x != 0; x = cdr(x)) {
        sx_t y = car(x);
        if (sx_type(y) == SX_TYPE_SYM)  continue;
        sx_eval(&work[-2], y);
    }
    
    sx_move(DST, &work[-2]);
}


static void cf_progn(void)
{
    sx_t *work = eval_alloc(1);

    sx_t args;
    for (args = ARGS; args != 0; args = cdr(args)) {
        sx_eval(&work[-1], car(args));
    }

    sx_move(DST, &work[-1]);
}


struct frame_env *module_current(void)
{
    struct frame_env *e;
    for (e = ENVFP; e != 0; e = e->up) {
        if (e->module != 0)  return (e);
    }

    assert(0);

    return (0);
}


void enter(sx_t *dst, sx_t locals_dict, sx_t env_dict, sx_t module, sx_t body)
{
    struct vm_state_save vmsave[1];
    vm_state_save(vmsave);
    
    sx_t *work = eval_alloc(1);    
    
    frame_env_push_module(env_dict, module);
    
    if (locals_dict != 0)  frame_env_push(locals_dict);

    for (; body != 0; body = cdr(body)) {
        sx_eval(&work[-1], car(body));
    }

    sx_move(dst, &work[-1]);

    vm_state_restore(vmsave);
}


void cf_dict_enter(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DICT)  except_bad_arg(x);
    enter(DST, 0, x, 0, cdr(ARGS));
}


void cf_module_enter(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_MODULE)  except_bad_arg(x);
    enter(DST, 0, x->u.moduleval->dict, x, cdr(ARGS));
}

/*
<doc>
## progn-up
### Type
nsubr
### Form
(up 
### Description
Returns _sexpr_, unevaluated
### Return value
Same as first argument
### Exceptions
None
### See also
### Examples
> -> (quote foo)  
> foo
</doc>
*/

static void cf_progn_up(void)
{
    struct frame_env *e = env_up_except(ENVFP);
    enter(DST, 0, e->dict, e->module, ARGS);
}


static void cf_env_current(void)
{
    cf_argc_chk(1);

    sx_assign(DST, ENVFP->dict);
}


static void cf_env_up(void)
{
    cf_argc_chk(2);

    sx_t x = cadr(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    int n = x->u.intval;
    if (n < 0)  except_bad_arg(x);
    struct frame_env *e;
    for (e = ENVFP; n > 0; --n, e = env_up_except(e));
    
    sx_assign(DST, e->dict);
}


static void cf_let(void)
{
    cf_argc_chk_min(1);
    
    sx_t *work = eval_alloc(2);

    struct sx_setval *d = dict_new(&work[-1], 16)->u.setval;

    sx_t x;
    for (x = car(ARGS); x != 0; x = cdr(x)) {
        if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(car(ARGS));
        sx_t y = car(x);
        switch (sx_type(y)) {
        case SX_TYPE_SYM:
            dict_atsput(d, y, sx_hash(y), 0);
            break;

        case SX_TYPE_DPTR:
            if (!list_len_chk(y, 2))  except_bad_arg(car(ARGS));
            sx_eval(&work[-2], cadr(y));
            env_bind_dict(work[-1], car(y), work[-2]);

            break;

        default:
            except_bad_arg(car(ARGS));
        }
    }

    frame_env_push(work[-1]);

    sx_assign_nil(&work[-2]);
    for (x = cdr(ARGS); x != 0; x = cdr(x)) {
        sx_eval(&work[-2], car(x));
    }

    sx_move(DST, &work[-2]);
}


static void cf_leta(void)
{
    cf_argc_chk_min(1);

    sx_t *work = eval_alloc(2);
    
    frame_env_push(dict_new(&work[-1], 16));
    sx_t x;
    for (x = car(ARGS); x != 0; x = cdr(x)) {
        sx_t y = car(x);
        switch (sx_type(y)) {
        case SX_TYPE_SYM:
            env_bind(y, 0);
            break;

        case SX_TYPE_DPTR:
            {
                if (!list_len_chk(y, 2))  except_bad_arg(car(ARGS));
                sx_eval(&work[-2], cadr(y));
                env_bind(car(y), work[-2]);
            }
            break;

        default:
            except_bad_arg(car(ARGS));
        }
    }

    for (x = cdr(ARGS); x != 0; x = cdr(x)) {
        sx_eval(&work[-2], car(x));
    }

    sx_move(DST, &work[-2]);
}


void except_goto_no_prog(void)
{
    except_raise1();
    
    except_newv(STR_CONST("system.goto-no-prog"), 0);

    frame_except_raise();
}


static void cf_goto(void)
{
    cf_argc_chk(1);
    if (PROGFP == 0 || PROGFP->arg == 0)  except_goto_no_prog();
    frame_prog_goto(car(ARGS));
}


void except_return_no_prog(void)
{
    except_raise1();
    
    except_newv(STR_CONST("system.return-no-prog"), 0);

    frame_except_raise();
}


static void cf_return(void)
{
    cf_argc_chk_range(0, 1);
    if (PROGFP == 0 || PROGFP->arg == 0)  except_return_no_prog();
    frame_prog_return(ARGC == 0 ? 0 : car(ARGS));
}


static void cf_cond(void)
{
    sx_t *work = eval_alloc(1);
    
    sx_t x;
    for (x = ARGS; x != 0; x = cdr(x)) {
        sx_t clause = car(x);
        sx_eval(&work[-1], car(clause));
        if (work[-1] == 0)  continue;
        for (clause = cdr(clause); clause != 0; clause = cdr(clause)) {
            sx_eval(&work[-1], car(clause));
        }

        break;
    }

    sx_move(DST, &work[-1]);
}


static void cf_if(void)
{
    cf_argc_chk_min(2);
    sx_t args = ARGS;
    sx_t x = car(args);
    args = cdr(args);
    
    sx_t *work = eval_alloc(1);

    sx_eval(&work[-1], x);
    if (work[-1] == 0) {
        if (ARGC < 3) {
            sx_assign_nil(DST);
        } else {
            for (x = cdr(args); x != 0; x = cdr(x)) {
                sx_eval(DST, car(x));
            }
        }
    } else {
        sx_eval(DST, car(args));
    }
}


static void cf_while(void)
{
    cf_argc_chk_min(1);

    sx_t *work = eval_alloc(1);

    frame_while_push();
    FRAME_LONGJMP_SETJMP(WHILEFP);
    switch (WHILEFP->jc) {
    case FRAME_LONGJMP_JC_NONE:
    case FRAME_LONGJMP_JC_WHILE_CONTINUE:  break;
    case FRAME_LONGJMP_JC_WHILE_BREAK:     goto done;
    default:                    assert(0);
    }

    for (;;) {
        sx_eval(&work[-1], car(ARGS));
        if (work[-1] == 0)  break;
        sx_t body;
        for (body = cdr(ARGS); body != 0; body = cdr(body)) {
            sx_eval(&work[-1], car(body));
        }
    }
    
 done:
    sx_assign_nil(DST);
}


void except_break_no_while(void)
{
    except_raise1();
    
    except_newv(STR_CONST("system.break-no-while-or-foreach"), 0);

    frame_except_raise();
}


static void cf_break(void)
{
    cf_argc_chk(0);
    if (WHILEFP == 0)  except_break_no_while();
    frame_while_break();
}


void except_continue_no_while(void)
{
    except_raise1();
    
    except_newv(STR_CONST("system.continue-no-while-or-foreach"), 0);

    frame_except_raise();
}


static void cf_continue(void)
{
    cf_argc_chk(0);
    if (WHILEFP == 0)  except_continue_no_while();
    frame_while_continue();
}


static void cf_eq(void)
{
    cf_argc_chk(2);
    bool_new(DST, car(ARGS) == cadr(ARGS));
}


static void cf_not(void)
{
    cf_argc_chk(1);
    bool_new(DST, car(ARGS) == 0);
}


static void cf_and(void)
{
    sx_t *work = eval_alloc(1);
    
    sx_assign(&work[-1], main_consts.t);
    sx_t args;
    for (args = ARGS; args != 0; args = cdr(args)) {
        sx_eval(&work[-1], car(args));
        if (work[-1] == 0)  break;
    }

    sx_move(DST, &work[-1]);
}


static void cf_or(void)
{
    sx_t *work = eval_alloc(1);

    sx_t args;
    for (args = ARGS; args != 0; args = cdr(args)) {
        sx_eval(&work[-1], car(args));
        if (work[-1] != 0)  break;
    }

    sx_move(DST, &work[-1]);
}


static void cf_obj_hash(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (x != 0)  except_bad_arg(x);
    
    int_new(DST, 0);
}


static void cf_obj_equal(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (x != 0)  except_bad_arg(x);

    bool_new(DST, cadr(ARGS) == 0);
}


static void cf_obj_append(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (x != 0)  except_bad_arg(x);

    cons(DST, cadr(ARGS), 0);
}


static void cf_complex_abs(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_COMPLEX)  except_bad_arg(x);

    float_new(DST, sqrtl(x->u.complexval->re * x->u.complexval->re
                         + x->u.complexval->im * x->u.complexval->im
                         )
              );
}


static void cf_complex_arg(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_COMPLEX)  except_bad_arg(x);
    
    float_new(DST, atan2l(x->u.complexval->im, x->u.complexval->re));
}


static void cf_complex_re(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_COMPLEX)  except_bad_arg(x);

    float_new(DST, x->u.complexval->re);
}


static void cf_complex_im(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_COMPLEX)  except_bad_arg(x);

    float_new(DST, x->u.complexval->im);
}


static void cf_int_mod(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);

    int_new(DST, x->u.intval % y->u.intval);
}


static void cf_int_minus(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    
    int_new(DST, -x->u.intval);
}


static void cf_int_bitand(void)
{
    intval_t result = (intval_t) -1;
    sx_t     args;
    unsigned argc;
    for (args = ARGS, argc = ARGC; argc > 0; --argc, args = cdr(args)) {
        sx_t x = car(args);
        if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
        result &= x->u.intval;
    }

    int_new(DST, result);
}


static void cf_int_bitor(void)
{
    intval_t result = 0;
    sx_t     args;
    unsigned argc;
    for (args = ARGS, argc = ARGC; argc > 0; --argc, args = cdr(args)) {
        sx_t x = car(args);
        if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
        result |= x->u.intval;
    }

    int_new(DST, result);
}


static void cf_int_bitnot(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    int_new(DST, ~x->u.intval);
}


static void sh_args(intval_t *val, intval_t *sh)
{
    cf_argc_chk_range(1, 2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    *val = x->u.intval;
    if (ARGC < 2) {
        *sh = 1;

        return;
    }

    x = cadr(ARGS);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    *sh = x->u.intval;
    if (*sh < 0)  except_bad_arg(x);
}


static void cf_int_lsh(void)
{
    intval_t val, sh;
    sh_args(&val, &sh);
    int_new(DST, val << sh);
}


static void cf_int_rsh(void)
{
    intval_t val, sh;
    sh_args(&val, &sh);
    int_new(DST, val >> sh);
}


static void cf_int_ursh(void)
{
    intval_t val, sh;
    sh_args(&val, &sh);
    int_new(DST, (unsigned long long) val >> sh);
}


static void cf_dptr_car(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(x);
    
    sx_assign(DST, car(x));
}


static void cf_dptr_cadr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(x);
    sx_t y = cdr(x);
    if (sx_type(y) != SX_TYPE_DPTR)  except_bad_arg(x);
    
    sx_assign(DST, car(y));
}


static void cf_dptr_cdr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    switch (sx_type(x)) {
    case SX_TYPE_NIL:
        sx_assign_nil(DST);
        break;

    case SX_TYPE_DPTR:
        sx_assign(DST, cdr(x));
        break;

    default:
        except_bad_arg(x);
    }
}


sx_t list_at(sx_t li, unsigned idx)
{
    for (; idx > 0; --idx, li = cdr(li));
    
    return (car(li));
}


static void cf_mapcar(void)
{
    cf_argc_chk_min(2);
    sx_t args = ARGS;
    sx_t x = car(args);
    args = cdr(args);
    int argc = ARGC - 1;

    sx_t *work = eval_alloc(2);

    method_find_except(&work[-1], x, main_consts.calln);
    struct sx_arrayval *a = array_new(&work[-2], argc)->u.arrayval;
    sx_t *p;
    for (p = a->data; args != 0; args = cdr(args), ++p) {
        sx_togenerator(p, car(args));
    }

    eval_alloc(2);

    sx_t *dst = DST;
    sx_assign_nil(dst);
    for (;;) {
        sx_assign_nil(&work[-3]);
        unsigned k;
        bool f = false;
        for (p = a->data + argc - 1, k = argc; k > 0; --k, --p) {
            sx_next(&work[-4], *p);
            if (work[-4] != 0) {
                f = true;

                sx_assign(&work[-4], car(work[-4]));
            }
            
            cons(&work[-3], work[-4], work[-3]);
        }
        if (!f)  break;

        cons(&work[-3], x, work[-3]);
        method_call_run(&work[-4], work[-1], 1 + argc, work[-3], ARGS_EVAL_SUPPRESS_ALL);
        list_append(&dst, work[-4]);
    }
}


static void cf_outer(void)
{
    cf_argc_chk_min(2);
    sx_t args = ARGS;
    sx_t x = car(args);
    args = cdr(args);
    int argc = ARGC - 1;

    sx_t *work = eval_alloc(2);

    method_find_except(&work[-1], x, main_consts.calln);

    /* work[-2] = iters */
    
    struct sx_arrayval *iters = array_new(&work[-2], argc)->u.arrayval;
    sx_t *p;
    for (p = iters->data; args != 0; args = cdr(args), ++p) {
        sx_togenerator(p, car(args));
    }

    /* work[-3] = current values */

    eval_alloc(1);

    struct sx_arrayval *vals = array_new(&work[-3], argc)->u.arrayval;
    sx_t *q;
    unsigned n;
    for (q = vals->data, p = iters->data, n = argc; n > 0; --n, ++p, ++q) {
        sx_next(q, *p);
    }
    
    sx_t *dst = DST;
    sx_assign_nil(dst);

    eval_alloc(2);
    
    for (;;) {
        sx_assign_nil(&work[-4]);
        p = &work[-4];
        for (q = vals->data, n = argc; n > 0; --n, ++q) {
            sx_t y = *q;
            if (y == 0)  continue;
            
            list_append(&p, car(y));
        }
        if (work[-4] == 0)  break;

        cons(&work[-4], x, work[-4]);
        method_call_run(&work[-5], work[-1], 1 + argc, work[-4], ARGS_EVAL_SUPPRESS_ALL);
        list_append(&dst, work[-5]);

        for (p = iters->data + argc - 1, q = vals->data + argc - 1, n = argc; n > 0; --n, --q, --p) {
            sx_next(q, *p);
            if (*q != 0)  break;

            method_call1_internal(&work[-5], main_consts.reset, *p);
            sx_next(q, *p);
        }
        if (n == 0)  break;
    }
}


void iterate_list_begin(sx_t *dst, sx_t sx)
{
    method_call1_internal(dst, main_consts.tolist, sx);
}


void iterate_callable_begin(sx_t *dst, sx_t sx)
{
    sx_t *work = eval_alloc(1);
    
    method_find_except(&work[-1], sx, main_consts.calln);
    cons(dst, work[-1], cons(dst, sx, 0));

    eval_unwind(work);
}


bool iterate_list_next(sx_t *dst, sx_t *sx)
{
  sx_t x = *sx;
  if (x == 0)  return (false);
  sx_assign(dst, car(x));
  sx_assign(sx, cdr(x));
        
  return (true);
}


bool iterate_callable_next(sx_t *dst, sx_t sx)
{
    bool result = false;
            
    sx_t *work = eval_alloc(1);
    
    method_call_run(&work[-1], car(sx), 1, cdr(sx), ARGS_EVAL_SUPPRESS_ALL);
    if (work[-1] != 0) {
        if (sx_type(work[-1]) != SX_TYPE_DPTR)  except_bad_value(work[-1]);
        sx_assign(dst, car(work[-1]));
        
        result = true;
    }
    
    eval_unwind(work);
    
    return (result);
}


static void cf_reduce_tolist(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);

    sx_t *work = eval_alloc(2);

    iterate_list_begin(&work[-1], x);
    
    args = cdr(args);  sx_t y = car(args);
    method_find_except(&work[-2], y, main_consts.calln);

    args = cdr(args);
    
    eval_alloc(1);

    sx_assign(DST, car(args));
    while (iterate_list_next(&work[-3], &work[-1])) {
        cons(&work[-3], y, cons(&work[-3], *DST, cons(&work[-3], work[-3], 0)));
        method_call_run(DST, work[-2], 3, work[-3], ARGS_EVAL_SUPPRESS_ALL);
    }
}


static void cf_reduce_callable(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);

    sx_t *work = eval_alloc(2);

    iterate_callable_begin(&work[-1], x);
    
    args = cdr(args);  sx_t y = car(args);
    method_find_except(&work[-2], y, main_consts.calln);

    args = cdr(args);
    
    eval_alloc(1);

    sx_assign(DST, car(args));
    while (iterate_callable_next(&work[-3], work[-1])) {
        cons(&work[-3], y, cons(&work[-3], *DST, cons(&work[-3], work[-3], 0)));
        method_call_run(DST, work[-2], 3, work[-3], ARGS_EVAL_SUPPRESS_ALL);
    }
}


static void cf_filter(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);

    sx_t *work = eval_alloc(4);

    method_find_except(&work[-1], x, main_consts.calln);
    sx_tolist(&work[-2], cadr(ARGS));
    sx_t *dst = DST;
    sx_assign_nil(dst);
    sx_t y;
    for (y = work[-2]; y != 0; y = cdr(y)) {
        sx_t z = car(y);
        cons(&work[-4], x, cons(&work[-4], z, 0));
        method_call_run(&work[-3], work[-1], 2, work[-4], ARGS_EVAL_SUPPRESS_ALL);
        if (work[-3] == 0)  continue;
        
        list_append(&dst, z);
    }
}


static void cf_filter_alt(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (list_len(x) < 0)  except_bad_arg(x);

    sx_t *work = eval_alloc(1);

    sx_tolist(&work[-1], cadr(ARGS));
    sx_t *dst = DST;
    sx_assign_nil(dst);
    sx_t y;
    for (y = work[-1]; y != 0; y = cdr(y), x = cdr(x)) {
        if (x == 0)  break;
        if (car(x) == 0)  continue;
    
        list_append(&dst, car(y));
    }
}


static sx_t module_name(sx_t *dst, sx_t filename)
{
    const char *s = filename->u.strval->data;
    const char *p = rindex(s, '/');
    p = (p == 0) ? s : p + 1;
    unsigned modname_size = strlen(p) + 1;
    if (modname_size > 4 && strcmp(p + modname_size - 4, ".so") == 0) {
        modname_size -= 3;
    }
    char *modname = mem_alloc(modname_size); /* Safe, no exceptions can occur */
    memcpy(modname, p, modname_size - 1);
    modname[modname_size - 1] = 0;

    return (str_newm(dst, modname_size, modname));
}


static void except_load_lib_failed(void)
{
    char *mesg = dlerror();
    
    sx_t *work = eval_alloc(1);

    except_raise1();

    except_newl(STR_CONST("system.module-load-failed"), 1, str_newc1(&work[-1], mesg));

    frame_except_raise();
}


static void path_find_unsafe(sx_t *dst, sx_t filename, sx_t path)
{
    sx_assign(dst, filename);

    struct sx_strval *f = filename->u.strval;
    if (f->size >= 2) {
        switch (f->data[0]) {
        case '.':
        case '/':
            return;
        default: ;
        }
    }

    if (sx_type(path) != SX_TYPE_DPTR)  return;

    for (; path != 0; path = cdr(path)) {
        if (sx_type(path) != SX_TYPE_DPTR)  break;
        sx_t x = car(path);
        if (sx_type(x) != SX_TYPE_STR)  continue;
        struct sx_strval *y = x->u.strval;
        unsigned bufsize = y->size + f->size;
        char buf[bufsize];
        snprintf(buf, bufsize, "%s/%s", y->data, f->data);
        if (access(buf, R_OK) == 0) {
            str_newc(dst, bufsize, buf);

            return;
        }
    }
}


static void load_lib(sx_t *dst, sx_t filename, sx_t sha1)
{
    struct sx_strval *s = filename->u.strval;
    void *dlhdl = dlopen(s->data, RTLD_NOW);
    if (dlhdl == 0)  except_load_lib_failed();

    sx_t *work = eval_alloc(2);
    
    struct sx_strval *modname = module_name(&work[-2], filename)->u.strval;

    unsigned symsize = 2 + modname->size - 1 + 7 + 1;
    char sym[symsize];
    snprintf(sym, symsize, "__%s_init__", modname->data);
    void (*f)(unsigned, const char *) = (void (*)(unsigned, const char *)) dlsym(dlhdl, sym);
    if (f == 0) {
        fprintf(stderr, "%s\n", dlerror());
        dlclose(dlhdl);
        sx_assign_nil(dst);
    } else {
        dict_new(&work[-1], 32);

        struct vm_state_save vmsave[1];
        vm_state_save(vmsave);
        
        frame_env_push(work[-1]);
        
        (*f)(modname->size, modname->data);
        
        vm_state_restore(vmsave);

        module_new(dst, work[-1], work[-2], sha1, dlhdl);        
    }

    eval_unwind(work);
}


static void load_file(sx_t *dst, sx_t filename, sx_t sha1)
{
    sx_t *work = eval_alloc(1);

    static const char mode[] = "r";
    str_newb(&work[-1], STR_CONST(mode));
    FILE *fp = fopen(filename->u.strval->data, mode);
    if (fp == 0)  except_file_open_failed(filename, work[-1]);
    file_new(&work[-1], filename, work[-1], fp);

    eval_alloc(3);
    
    module_new(&work[-2], dict_new(&work[-3], 64), module_name(&work[-4], filename), sha1, 0);

    struct vm_state_save vmsave[1];
    vm_state_save(vmsave);

    frame_input_push(work[-1], NON_INTERACTIVE);
    frame_env_push_module(work[-3], work[-2]);

    eval_alloc(1);
    
    while (sx_read(&work[-4]))  sx_eval(&work[-5], work[-4]);
    
    vm_state_restore(vmsave);

    sx_move(dst, &work[-2]);
    
    eval_unwind(work);
}


static struct sx_setval *modules_loaded_dict(void)
{
    sx_t pr = dict_at(main_consts.Module->u.classval->class->class_vars->u.setval,
                      main_consts.__loaded__, sx_hash(main_consts.__loaded__)
                      );
    if (pr == 0)  fatal("Internal error -- loaded-modules Dict not found");
    return (cdr(pr)->u.setval);
}


static void except_system_err(unsigned mesg_size, const char *mesg)
{
    except_raise1();

    sx_t *work = eval_alloc(1);

    str_newc(&work[-1], mesg_size, mesg);
    except_newl(STR_CONST("system.system-error"), 1, work[-1]);

    frame_except_raise();
}


static void cf_module_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(ARGS);
    if (!sx_is_str(x))  except_bad_arg(x);
    
    sx_t *work = eval_alloc(2);

    sx_t p = dict_ats(main_consts.Module->u.classval->class->class_vars->u.setval,
                      main_consts.path,
                      sx_hash(main_consts.path)
                      );
    if (p != 0) {
        path_find_unsafe(&work[-1], x, cdr(p));
    } else {
        sx_assign(&work[-1], x);
    }

    struct sx_setval *_modules_loaded_dict = modules_loaded_dict();

    struct sx_strval *s = work[-1]->u.strval;
    static const char cmd_ldr[] = "sha1sum -b ";
    static const char cmd_trlr[] = " 2>/dev/null";
    unsigned bufsize = sizeof(cmd_ldr) - 1 + s->size - 1 + sizeof(cmd_trlr) - 1 + 1;
    char buf[bufsize];
    snprintf(buf, bufsize, "%s%s%s", cmd_ldr, s->data, cmd_trlr);
    FILE *fp = popen(buf, "r");
    static const char sha1_failed_mesg[] = "Failed to compute module checksum";
    if (fp == 0)  except_system_err(STR_CONST(sha1_failed_mesg));
    enum { SHA1_BUFSIZE = 41 };
    char *sha1buf = mem_alloc(SHA1_BUFSIZE);
    unsigned n = fread(sha1buf, 1, 40, fp);
    pclose(fp);
    if (n != 40) {
        mem_free(SHA1_BUFSIZE, sha1buf);
        except_system_err(STR_CONST(sha1_failed_mesg));
    }
    sha1buf[40] = 0;    
    str_newm(&work[-2], 41, sha1buf);
    sx_t pr = dict_at(_modules_loaded_dict, work[-2], sx_hash(work[-2]));
    if (pr != 0) {
        /* Already loaded */
        
        sx_assign(DST, cdr(pr));

        return;
    }

    struct sx_strval *ss = work[-1]->u.strval;
    if (ss->size >= 4 && strcmp((char *) &ss->data[ss->size - 4], ".so") == 0) {
        load_lib(DST, work[-1], work[-2]);
    } else {
        load_file(DST, work[-1], work[-2]);
    }

    dict_atput(_modules_loaded_dict, work[-2], sx_hash(work[-2]), *DST);
}


static void module_free(sx_t sx)
{
    struct sx_moduleval *m = sx->u.moduleval;
    dict_del(modules_loaded_dict(), m->sha1, sx_hash(m->sha1));    
}


static void cf_module_current(void)
{
    cf_argc_chk(1);
    struct frame_env *e = module_current();
    if (e == 0) {
        sx_assign_nil(DST);

        return;
    }

    sx_assign(DST, e->module);
}


static void cf_module_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_MODULE)  except_bad_arg(x);

    struct sx_strval *s1 = sx_inst_of_class_name(x);
    struct sx_strval *s2 = x->u.moduleval->name->u.strval;
    
    unsigned bufsize = 1 + s1->size - 1 + 2 + s2->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
    snprintf(buf, bufsize, "<%s: %s>", s1->data, s2->data);
    str_newm(DST, bufsize, buf);
}


static void cf_array_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(ARGS);
    if (sx_type(x) == SX_TYPE_INT) {
        intval_t size = x->u.intval;
        if (size  < 0)  except_bad_arg(x);
        array_new(DST, size);

        return;
    }
    if (sx_type(x) == SX_TYPE_ARRAY) {
        array_copy(DST, x);

        return;
    }

    sx_t *work = eval_alloc(1);

    sx_tolist(&work[-1], x);
    int size = list_len(work[-1]);
    sx_t a = array_new(DST, size);
    sx_t *p;
    for (p = a->u.arrayval->data, x = work[-1]; x != 0; x = cdr(x), ++p) {
        sx_assign(p, car(x));
    }
}


static void cf_array_copy(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);

    array_copy(DST, x);
}


static void cf_array_copydeep(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);

    sx_t *work = eval_alloc(1);
    
    array_copydeep_unsafe(&work[-1], x);

    sx_move(DST, &work[-1]);
}


static unsigned repr_init_size(sx_t sx)
{
    return (5          /* Fixed portion, "<" + ": " + ">" + terminator */
            + sx_inst_of(sx)->u.classval->class->name->u.strval->size - 1 /* Class name */
            + (sx->flags.frozen ? 1 : 0)                                  /* "!" if frozen */
            );
}

static void repr_from_list(sx_t *dst, sx_t sx, unsigned size, sx_t li)
{
    char *buf = mem_alloc(size); /* Safe, no exceptions can occur */
    FILE *fp = fmemopen(buf, size, "w");
    fprintf(fp, "<%s%s: ",
            sx_inst_of(sx)->u.classval->class->name->u.strval->data,
            sx->flags.frozen ? "!" : ""
            );
    if (li == 0) {
        fprintf(fp, "%s", main_consts.nil->u.strval->data);
    } else {
        fprintf(fp, "'(");
        bool f = false;
        for (; li != 0; li = cdr(li), f = true) {
            if (f)  fprintf(fp, " ");
            fprintf(fp, car(li)->u.strval->data);
        }
        fprintf(fp, ")");
    }
    fprintf(fp, ">");
    fclose(fp);

    str_newm(DST, size, buf);
}


static void cf_array_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);
    struct sx_arrayval *a = x->u.arrayval;

    unsigned size = repr_init_size(x);

    sx_t *work = eval_alloc(2);

    if (a->size == 0) {
        size += main_consts.nil->u.strval->size - 1; /* "nil" */
    } else {
        size += 3;              /* "'(" + ")" */
        sx_t *p = &work[-1];
        sx_t *q;
        unsigned n;
        bool f = false;
        for (q = a->data, n = a->size; n > 0; --n, ++q, f = true) {
            if (f)  ++size;     /* " " */
            struct sx_strval *s = sx_repr(&work[-2], *q);
            list_append(&p, work[-2]);
            size += s->size - 1;
        }
    }

    repr_from_list(DST, x, size, work[-1]);
}


static void cf_array_tolist(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);

    sx_t *work = eval_alloc(1), *p = &work[-1];

    struct sx_arrayval *a = x->u.arrayval;
    sx_t *q;
    unsigned k;
    for (q = a->data, k = a->size; k > 0; --k, ++q) {
        list_append(&p, *q);
    }

    sx_move(DST, &work[-1]);

    eval_unwind(work);
}


void except_index_range(sx_t idx)
{
    except_raise1();
    
    except_newl(STR_CONST("system.index-range"), 1, idx);

    frame_except_raise();
}


void except_index_range2(sx_t ofs, sx_t len)
{
    except_raise1();
    
    except_newl(STR_CONST("system.index-range"), 2, ofs, len);

    frame_except_raise();
}


static void cf_array_at(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);

    struct sx_arrayval *a = x->u.arrayval;
    intval_t idx = y->u.intval;
    if (!slice1(&idx, a->size))  except_index_range(y);
    
    sx_assign(DST, a->data[idx]);
}


static void cf_array_atput(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    args = cdr(args);  sx_t z = car(args);

    struct sx_arrayval *a = x->u.arrayval;
    intval_t idx = y->u.intval;
    if (!slice1(&idx, a->size))  except_index_range(y);
    
    sx_assign(&a->data[idx], z);

    sx_assign(DST, z);
}


static void arr_bsort(unsigned size, sx_t *data, sx_t cmp)
{
    if (size <= 1)  return;

    unsigned n, i, k;
    for (n = size - 1; n > 0; --n) {
        unsigned f = false;
        for (i = 0, k = n; k > 0; --k, ++i) {
            if (sx_cmp_with_callable(data[i], data[i + 1], cmp) <= 0)  continue;
            sx_swap(&data[i], &data[i + 1]);
            f = true;
        }
        if (!f)  break;
    }
}


static void arr_qsort(unsigned size, sx_t *data, sx_t cmp)
{
    if (size <= 6) {
        arr_bsort(size, data, cmp);

        return;
    }

    sx_t pivot_value = data[0];
    unsigned pivot_idx = 0;
    unsigned i = 1, j = size - 1;
    for (;;) {
        bool f_lo = false;
        for (; i < j; ++i) {
            if (sx_cmp_with_callable(data[i], pivot_value, cmp) > 0) {
                f_lo = true;
                break;
            }
        }
        for (; i < j; --j) {
            if (sx_cmp_with_callable(data[j], pivot_value, cmp) < 0)  break;
        }

        if (i >= j) {
            DEBUG_ASSERT(i == j);
            if (f_lo) {
                /* i goes after pivot, j ran into i */

                pivot_idx = i - 1;

                break;
            }

            /* i ran into j; N.B. j not considered yet */
            pivot_idx = sx_cmp_with_callable(data[j], pivot_value, cmp) > 0 ? j - 1 : j;

            break;
        }
        
        sx_swap(&data[i], &data[j]);
        ++i;  --j;

        if (i > j) {
            DEBUG_ASSERT(i == (j + 1));

            pivot_idx = j;

            break;
        }
    }

    sx_swap(&data[0], &data[pivot_idx]);
    
    arr_qsort(pivot_idx, data, cmp);
    arr_qsort(size - (pivot_idx + 1), &data[pivot_idx + 1], cmp);
}

#ifndef NDEBUG

void arr_sort_chk(unsigned size, sx_t *data, sx_t cmp)
{
    unsigned i, j;
    for (i = 0, j = 1; j < size; ++i, ++j) {
        assert(sx_cmp_with_callable(data[i], data[j], cmp) <= 0);
    }
}

#endif

static void cf_array_sort(void)
{
    cf_argc_chk_range(1, 2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);

    struct sx_arrayval *a = x->u.arrayval;
    arr_qsort(a->size, a->data, ARGC == 1 ? 0 : cadr(ARGS));

#ifndef NDEBUG
    arr_sort_chk(a->size, a->data, ARGC == 1 ? 0 : cadr(ARGS));
#endif
    
    sx_assign(DST, x);
}


static void cf_array_size(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAY)  except_bad_arg(x);

    int_new(DST, x->u.arrayval->size);
}


static void cf_barray_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(ARGS);
    if (sx_type(x) == SX_TYPE_INT) {
        intval_t size = x->u.intval;
        if (size  < 0)  except_bad_arg(x);
        barray_newc(DST, size, 0);

        return;
    }

    if (sx_is_str(x)) {
        struct sx_strval *s = x->u.strval;
        barray_newc(DST, s->size - 1, (unsigned char *) s->data);

        return;
    }
    
    int size = list_len(x);
    if (size >= 0) {
        unsigned char *p = barray_newc(DST, size, 0)->u.barrayval->data;
        for (; x != 0; x = cdr(x), ++p) {
            sx_t y = car(x);
            if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(x);
            intval_t yv = y->u.intval;
            if (!(yv >= 0 && yv <= 255))  except_bad_arg(x);
            *p = yv;
        }

        return;
    }

    except_bad_arg(x);
}


static void cf_barray_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAY)  except_bad_arg(x);
    struct sx_barrayval *b = x->u.barrayval;

    unsigned size = repr_init_size(x);

    sx_t *work = eval_alloc(2);

    if (b->size == 0) {
        size += main_consts.nil->u.strval->size - 1; /* "nil" */
    } else {
        size += 3;              /* "'(" + ")" */
        sx_t *p = &work[-1];
        unsigned char *q;
        unsigned n;
        bool f = false;
        for (q = b->data, n = b->size; n > 0; --n, ++q, f = true) {
            if (f)  ++size;     /* " " */
            char buf[4];
            snprintf(buf, sizeof(buf), "%u", *q);
            str_newc1(&work[-2], buf);
            list_append(&p, work[-2]);
            size += work[-2]->u.strval->size - 1;
        }
    }

    repr_from_list(DST, x, size, work[-1]);
}


static void cf_barray_tostring(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAY)  except_bad_arg(x);
    struct sx_barrayval *b = x->u.barrayval;
    unsigned char *p;
    unsigned n, size = 1;
    for (p = b->data, n = b->size; n > 0; --n, ++p) {
        size += isprint(*p) ? 1 : 4;
    }
    char *buf = mem_alloc(size), *q; /* Safe, no exceptions can occur */
    for (q = buf, p = b->data, n = b->size; n > 0; --n, ++p) {
        unsigned char bb = *p;
        if (isprint(bb)) {
            *q++ = bb;

            continue;
        }

        snprintf(q, 5, "\\x%02x", bb);
        q += 4;
    }
    *q = 0;
    
    str_newm(DST, size, buf);
}


static void cf_barray_tolist(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAY)  except_bad_arg(x);
    struct sx_barrayval *b = x->u.barrayval;
            
    sx_t *work = eval_alloc(1);

    sx_t *dst = DST;
    sx_assign_nil(dst);
    unsigned char *q;
    unsigned k;
    for (q = b->data, k = b->size; k > 0; --k, ++q) {
        list_append(&dst, int_new(&work[-1], *q));
    }
    
    eval_unwind(work);
}


static void cf_barray_at(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAY)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    struct sx_barrayval *b = x->u.barrayval;
    intval_t ofs = y->u.intval;

    if (!slice1(&ofs, b->size))  except_index_range(y);

    int_new(DST, b->data[ofs]);
}


static void cf_barray_atput(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_BARRAY)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    args = cdr(args);  sx_t z = car(args);
    if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
    intval_t zi = z->u.intval;
    if (zi < 0 || zi > 255)  except_bad_arg(z);
    struct sx_barrayval *b = x->u.barrayval;
    intval_t ofs = y->u.intval;

    if (!slice1(&ofs, b->size))  except_index_range(y);

    b->data[ofs] = zi;

    sx_assign(DST, z);
}


static void cf_barray_copy(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAY)  except_bad_arg(x);

    barray_copy(DST, x);
}


static void cf_barray_size(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAY)  except_bad_arg(x);

    int_new(DST, x->u.barrayval->size);
}


static void cf_set_new(void)
{
    switch (ARGC) {
    case 1:
        set_new(DST, 0);
        break;

    case 2:
        {
            sx_t x = cadr(ARGS);
            if (sx_type(x) == SX_TYPE_INT) {
                intval_t size = x->u.intval;
                if (size  < 0)  except_bad_arg(x);
                set_new(DST, size);

                break;
            }
            if (list_len(x) >= 0) {
                struct sx_setval *s = set_new(DST, 0)->u.setval;
                for (; x != 0; x = cdr(x)) {
                    sx_t y = car(x);
                    set_put(s, y, sx_hash(y));
                }

                break;
            }

            except_bad_arg(x);
        }

        break;

    default:
        except_num_args_range(1, 2);
    }
}


static void cf_set_copy(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_set(x))  except_bad_arg(x);

    set_copy_unsafe(DST, x);    /* See cf node 1 */
}


static void cf_set_copydeep(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_set(x))  except_bad_arg(x);

    set_copydeep_unsafe(DST, x); /* See cf note 1 */
}


static void cf_set_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_set(x))  except_bad_arg(x);
    struct sx_setval *s = x->u.setval;

    unsigned size = repr_init_size(x);
    
    sx_t *work = eval_alloc(2);

    if (s->cnt == 0) {
        size += main_consts.nil->u.strval->size - 1; /* "nil" */
    } else {
        size += 3;              /* "'(" + ")" */
        struct sx_arrayval *a = s->base;
        sx_t *p = &work[-1];
        sx_t *q;
        unsigned n;
        bool f = false;
        for (q = a->data, n = a->size; n > 0; --n, ++q) {
            sx_t r;
            for (r = *q; r != 0; r = cdr(r), f = true) {
                if (f)  ++size;     /* " " */
                struct sx_strval *s = sx_repr(&work[-2], car(r));
                list_append(&p, work[-2]);
                size += s->size - 1;
            }
        }
    }

    repr_from_list(DST, x, size, work[-1]);
}

#define LIST_FOREACH_BEGIN(_li, _var)                                          \
  {                                                                            \
    sx_t __q;                                                                  \
    for (__q = _li; __q != 0; __q = cdr(__q)) {                                \
      sx_t _var = car(__q);

#define LIST_FOREACH_END  } }

#define SET_FOREACH_BEGIN(_s, _var)                                            \
  {                                                                            \
    struct sx_arrayval *__a = _s->u.setval->base;                              \
    sx_t *__q;                                                                 \
    unsigned __k;                                                              \
    for (__q = __a->data, __k = __a->size; __k > 0; --__k, ++__q) {            \
      sx_t __r;                                                                \
      for (__r = *__q; __r != 0; __r = cdr(__r)) {                             \
        sx_t _var = car(__r);

#define SET_FOREACH_END  } } }

static void cf_set_tolist(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_set(x))  except_bad_arg(x);

    sx_t *dst = DST;
    sx_assign_nil(dst);         /* See cf note 1 */
    SET_FOREACH_BEGIN(x, y) {
        list_append(&dst, y);
    } SET_FOREACH_END;
}


static void cf_set_at(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_SET)  except_bad_arg(x);
    sx_t y = cadr(ARGS);

    bool_new(DST, set_at(x->u.setval, y, sx_hash(y)));
}


static void cf_set_put(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_SET)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    
    set_put(x->u.setval, y, sx_hash(y));
    sx_assign(DST, y);
}


static void cf_set_size(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (!sx_is_set(x))  except_bad_arg(x);

    int_new(DST, x->u.setval->cnt);
}

/*
<doc>
## dict
### Type
subr
### Form
(dict _sx_)
### Description
Constructs or extracts a dictiontionary
#### When _sx_ is an int
Construct a dictionary whose hash table size is the given value, rounded up to the nearest power of 2.
#### When _sx_ is a list
Construct a dictionary from the given alist, ie. a list of dotted-pairs, with the car of the dotted-pair being the key, and the cdr being the value.
#### When _sx_ is a closure
Returns the dictonary associated with the given closure.
#### When _sx_ is an env
Returns this dictionary associated with the given environment
#### When _sx_ is a module
Returns the dictionary associated with the given module, i.e. the module's environment.
### Return value
dict
### Exceptions
system.bad-argument
### See also
### Examples
> -> (quote foo)  
> foo
</doc>
*/

static void cf_dict_new(void)
{
    cf_argc_chk_range(1, 2);
    if (ARGC == 1) {
        unsigned size = 0;
        do {
            sx_t x = dict_ats(main_consts.Dict->u.classval->class->class_vars->u.setval,
                              main_consts.default_size,
                              sx_hash(main_consts.default_size)
                              );
            if (x == 0)  break;
            x = cdr(x);
            if (sx_type(x) != SX_TYPE_INT)  break;
            intval_t i = x->u.intval;
            if (i < 0)  break;
            size = i;
        } while (0);
        
        dict_new(DST, size);

        return;
    }

    sx_t x = cadr(ARGS);
    switch (sx_type(x)) {
    case SX_TYPE_INT:
        {
            intval_t size = x->u.intval;
            if (size  < 0)  except_bad_arg(x);
            dict_new(DST, size);
        }
        return;

    case SX_TYPE_CLOSURE:
        sx_assign(DST, x->u.closureval->dict);
        return;

    case SX_TYPE_MODULE:
        sx_assign(DST, x->u.moduleval->dict);
        return;
        
    default:
        if (list_len(x) >= 0) {
            struct sx_setval *d = dict_new(DST, 0)->u.setval;
            for (; x != 0; x = cdr(x)) {
                sx_t y = car(x);
                if (sx_type(y) != SX_TYPE_DPTR)  except_bad_arg(cadr(ARGS));
                sx_t k = car(y);
                dict_atput(d, k, sx_hash(k), cdr(y));
            }

            return;
        }
    }

    except_bad_arg(x);
}


void except_no_member(sx_t obj, sx_t key)
{
    except_raise1();

    sx_t *work = eval_alloc(1);

    cons(&work[-1], obj, cons(&work[-1], key, 0));
    
    except_newv(STR_CONST("system.no-member"), work[-1]);
    
    frame_except_raise();
}


static sx_t _cf_dict_at(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DICT)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    
    return (dict_at(x->u.setval, y, sx_hash(y)));
}


static void cf_dict_at(void)
{
    sx_assign(DST, _cf_dict_at());
}


static void cf_dict_ate(void)
{
    sx_t pr = _cf_dict_at();
    if (pr == 0)  except_no_member(car(ARGS), cadr(ARGS));

    sx_assign(DST, cdr(pr));
}


static void cf_dict_at_dflt(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_DICT)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    sx_assign(DST, dict_at_dflt(x->u.setval, y, sx_hash(y), cadr(args)));
}


void except_frozen(sx_t sx)
{
    except_raise1();
    
    except_newl(STR_CONST("system.frozen"), 1, sx);

    frame_except_raise();
}


void except_not_frozen(sx_t sx)
{
    except_raise1();
    
    except_newl(STR_CONST("system.not-frozen"), 1, sx);

    frame_except_raise();
}


static void cf_dict_atput(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_DICT)  except_bad_arg(x);
    if (x->flags.frozen)  except_frozen(x);
    args = cdr(args);  sx_t y = car(args);
    args = cdr(args);  sx_t z = car(args);

    dict_atput(x->u.setval, y, sx_hash(y), z);
    
    sx_assign(DST, z);
}


static void cf_dict_put(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DICT)  except_bad_arg(x);
    if (x->flags.frozen)  except_frozen(x);
    sx_t y = cadr(ARGS);
    if (sx_type(y) != SX_TYPE_DPTR)  except_bad_arg(y);

    sx_t k = car(y);
    dict_atput(x->u.setval, k, sx_hash(k), cdr(y));
    
    sx_assign(DST, y);
}


static void cf_dict_del(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DICT)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    dict_del(x->u.setval, y, sx_hash(y));
    sx_assign(DST, y);
}


static sx_t _cf_module_at(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_MODULE)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (sx_type(y) != SX_TYPE_SYM)  except_bad_arg(y);

    return (dict_ats(x->u.moduleval->dict->u.setval, y, sx_hash(y)));
}


static void cf_module_at(void)
{
    sx_assign(DST, _cf_module_at());
}


static void cf_module_ate(void)
{
    sx_t pr = _cf_module_at();
    if (pr == 0)  except_no_member(car(ARGS), cadr(ARGS));

    sx_assign(DST, cdr(pr));
}

/* subr dst optimization to here */

static void cf_closure_new(void)
{
    cf_argc_chk(3);
    sx_t args = cdr(ARGS);
    sx_t x = car(args);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_DICT)  except_bad_arg(y);

    closure_new(DST, x, y);
}


void _cf_closure_copy(sx_t (*func)(sx_t *, sx_t))
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSURE)  except_bad_arg(x);
    struct sx_closureval *cl = x->u.closureval;

    sx_t *work = eval_alloc(1);

    closure_new(DST, cl->expr, (*func)(&work[-1], cl->dict));
}


static void cf_closure_copy(void)
{
    _cf_closure_copy(set_copy_unsafe);
}


static void cf_closure_copydeep(void)
{
    _cf_closure_copy(set_copydeep_unsafe);
}


static void cf_closure_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSURE)  except_bad_arg(x);

    sx_t *work = eval_alloc(2);

    struct sx_strval *clnm = sx_inst_of_class_name(x);
    struct sx_closureval *c = x->u.closureval;
    
    struct sx_strval *s1 = sx_repr(&work[-1], c->expr);
    struct sx_strval *s2 = sx_repr(&work[-2], c->dict);
    unsigned bufsize = 1 + clnm->size - 1 + 2 + s1->size - 1 + 1 + s2->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
    snprintf(buf, bufsize, "<%s: %s %s>", clnm->data, s1->data, s2->data);
    str_newm(DST, bufsize, buf);
            
    eval_unwind(work);
}


static void cf_closure_enter(void)
{
    cf_argc_chk_min(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSURE)  except_bad_arg(x);
    enter(DST, 0, x->u.closureval->dict, 0, cdr(ARGS));
}


void except_idx_range2(sx_t idx, sx_t len)
{
    except_raise1();

    except_newl(STR_CONST("system.index-range"), 2, idx, len);

    frame_except_raise();
}


void except_key_not_found(sx_t dict, sx_t key)
{
    except_raise1();

    except_newl(STR_CONST("system.key-not-found"), 2, dict, key);

    frame_except_raise();
}

/* <doc> **********************************************************************
## at
### Type
subr
### Form
(at _in_ _which_)
### Description
#### When _in_ is a symbol or string
Return character at offset _which_, which must be an integer, in string given by
_in_.  A negative value for _which_ is taken as an offset from the end of _in_.
#### When _in_ is a list
Return sexpr at offset _which_, which must be an integer, in list given by _in_.
A negative value for _which_ is taken as an offset from the end of _in_.
#### When _in_ is a barray
Return integer at offset _which_, which must be an integer, in barray given by
_in_.  A negative value for _which_ is taken as an offset from the end of _in_.
#### When _in_ is an array
Return sexpr at offset _which_, which must be an integer, in array given by
_in_.  A negative value for _which_ is taken as an offset from the end of _in_.
#### When _in_ is a set
Return t if _which_ is in set given by _in_, else return nil.
#### When _in_ is a dict
If key is in _in_, return dotted-pair for key _which_ in dict given by _in_;
otherwise, return nil.
#### When _in_ is a module
If key is in dict belonging to _in_, return dotted-pair for key _which_ in dict
given by _in_; otherwise, return nil.
### Return value
See above
### Exceptions
- system.index-range
### See also
ate
### Examples
> -> (at "The rain in Spain" 1)
> h
> -> (at '(99 100 101) 1)
> 100
> -> (at (barray '(99 100 101)) 1)
> 100
> -> (at (array '(99 foo (1 2 3))) 1)
> foo
> -> (at (set '(99 foo (1 2 3))) 99)
> t
> -> (at (dict '((foo . 123) (bar . 456))) 'bar)
> (bar . 456)
> -> (at (dict '((foo . 123) (bar . 456))) 'abc)
> nil
</doc>
<doc>
## ate
### Type
subr
### Form
(ate _in_ _which_)
### Description
This function is the same as the function "at", except when _in_ is a dict or a
module, in which case the function returns:
- the value associated with the key, if the key present; or
- a system.key-not-found exception is thrown, if the key is not present
### Return value
See above
### Exceptions
- system.index-range
- system.key-not-found
### See also
at
### Examples
> -> (ate "The rain in Spain" 1)  
> h  
> -> (ate '(99 100 101) 1)  
> 100  
> -> (ate (barray '(99 100 101)) 1)  
> 100  
> -> (ate (array '(99 foo (1 2 3))) 1)  
> foo  
> -> (ate (set '(99 foo (1 2 3))) 99)  
> t  
> -> (ate (dict '((foo . 123) (bar . 456))) 'bar)  
> 456  
** </doc> **********************************************************************/

/* <test> **********************************************************************
> (assert (equal (at "The rain in Spain" 2) "e"))
< t
> (assert (equal (at "The rain in Spain" -2) "i"))
< t
** </test> **********************************************************************/

static void cf_freeze(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    x->flags.frozen = true;

    sx_assign(DST, x);
}


static void cf_frozenp(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);

    bool_new(DST, x->flags.frozen);    
}


static void cf_true(void)
{
    cf_argc_chk(1);
    sx_assign(DST, main_consts.t);
}


static void cf_false(void)
{
    cf_argc_chk(1);
    sx_assign_nil(DST);
}


static void sx_copydeep(sx_t *dst, sx_t sx) /* Internal use only, arg eval suppressed */
{
    method_call1_internal(dst, main_consts.copydeep, sx);
}


static void cf_try(void)
{
    if (!(ARGC >= 2 && ARGC <= 3))  except_num_args_range(2, 3);
    sx_t args = ARGS;
    sx_t x = car(args);
    args = cdr(args);
    sx_t y = car(args);

    sx_t *work = eval_alloc(1);
    
    struct frame_longjmp *fr = frame_except_push(&work[-1]);
    FRAME_LONGJMP_SETJMP(fr);
    switch (fr->jc) {
    case FRAME_LONGJMP_JC_NONE:
        sx_eval(DST, x);

        frame_longjmp_pop(fr);

        if (ARGC == 3)  sx_eval(DST, cadr(args));

        break;

    case FRAME_LONGJMP_JC_EXCEPT_RAISE:
        {
            sx_t *work2 = eval_alloc(1);

            cons(&work2[-1], y, cons(&work2[-1], *fr->arg, 0));
            method_call_apply(DST, main_consts.calln, 2, work2[-1], ARGS_EVAL_SUPPRESS_ALL);
            
            frame_longjmp_pop(fr);
        }
        break;

    default:
        assert(0);
    }
}


static void cf_raise(void)
{
    if (ARGC < 1)  except_num_args_min(1);
    sx_t x = car(ARGS);
    if (!sx_is_str(x))  except_bad_arg(x);

    except_raise1();
    
    sx_assign(XFP->arg, ARGS);

    frame_except_raise();
}


void rep(void)
{
    struct vm_state_save vmsave[1];
    vm_state_save(vmsave);
    
    sx_t *work = eval_alloc(3);

    struct frame_longjmp *old_except_top = except_top;
    frame_except_push(&work[-1]);
    except_top = XFP;
    
    FRAME_LONGJMP_SETJMP(XFP);
    switch (XFP->jc) {
    case FRAME_LONGJMP_JC_NONE:
        break;

    case FRAME_LONGJMP_JC_EXCEPT_RAISE:
        if (!INFP->flags.interactive)  goto done;
        frame_input_reset();
        break;

    default:
        assert(0);
    }
    
    frame_rep_push();
    FRAME_LONGJMP_SETJMP(REPFP);
    if (REPFP->jc == FRAME_LONGJMP_JC_NONE) {
        for (;;) {
            if (INFP->flags.interactive) {
                fflush(stderr);
                fflush(stdout);
            }
            if (!sx_read(&work[-3])) {
                if (INFP->flags.interactive)  putchar('\n');
                
                goto done;
            }
            sx_eval(&work[-2], work[-3]);
#ifndef TEST
            if (INFP->flags.interactive)
#endif
            {
                sx_repr(&work[-3], work[-2]);
                fputs(work[-3]->u.strval->data, stdout);
                fputc('\n', stdout);
            }
            
            DEBUG_ASSERT(vm->sp == &work[-3]);
        }
    }

 done:
    except_top = old_except_top;

    vm_state_restore(vmsave);
}

#ifndef NDEBUG

void debug_rep(void)
{
    struct vm_state_save vmsave[1];
    vm_state_save(vmsave);

    sx_t *work = eval_alloc(4);

    static const char filename[] = "/dev/tty", mode[] = "r+";
    FILE *fp = fopen(filename, mode);
    assert(fp != 0);

    str_newb(&work[-1], STR_CONST(filename));
    str_newb(&work[-2], STR_CONST(mode));
    file_new(&work[-1], work[-1], work[-2], fp);

    frame_input_push(work[-1], INTERACTIVE);

    struct frame_longjmp *old_except_top = XFP;
    frame_except_push(&work[-2]);
    except_top = XFP;

    FRAME_LONGJMP_SETJMP(XFP);
    if (XFP->jc == FRAME_LONGJMP_JC_EXCEPT_RAISE) {
        while (fgetc(fp) != '\n');
    }
    
    frame_rep_push();
    FRAME_LONGJMP_SETJMP(REPFP);
    if (REPFP->jc == FRAME_LONGJMP_JC_NONE) {
        for (;;) {
            backtrace();
            fflush(stderr);
            fputs("\ndebug ", stdout);
            fflush(stdout);

            char cmd;
            if (fread(&cmd, 1, 1, fp) != 1)  goto done;
            switch (cmd) {
            case 's':
                debug_step();

                goto done;
                
            case 'n':
                debug_next();

                goto done;
                
            case 'c':
                goto done;

            case 'w':
                backtrace();

                break;

            case 'p':
                if (!sx_read(&work[-3])) {
                    putchar('\n');
                    
                    goto done;
                }
                sx_eval(&work[-2], work[-3]);
                sx_repr(&work[-3], work[-2]);
                fputs(work[-3]->u.strval->data, stdout);
                fputc('\n', stdout);
                
                DEBUG_ASSERT(vm->sp == &work[-3]);

                break;

            default:
                fputs("Unknown command\n", fp);
            }
        }
    }

 done:
    except_top = old_except_top;
    
    vm_state_restore(vmsave);
}

#endif /* !defined(NDEBUG) */

static void cf_dptr_new(void)
{
    cf_argc_chk(3);
    sx_t args = cdr(ARGS);
    cons(DST, car(args), cadr(args));
}


static void cf_list_new(void)
{
    cf_argc_chk(2);
    sx_tolist(DST, cadr(ARGS));
}


static void cf_list_dup(void)
{
    cf_argc_chk(3);
    sx_t x = cadr(ARGS), y = car(cdr(cdr(ARGS)));
    if (!(sx_type(x) == SX_TYPE_INT && x->u.intval >= 0))  except_bad_arg(x);

    sx_t *dst = DST;
    sx_assign_nil(dst);
    intval_t n;
    for (n = x->u.intval; n > 0; --n)  list_append(&dst, y);
}


static void cf_int_iota(void)
{
    cf_argc_chk_range(1, 3);
    sx_t args = ARGS;
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    intval_t cnt = x->u.intval;
    if (cnt < 0)  except_bad_arg(x);
    intval_t origin = 0;
    if (ARGC >= 2) {
        args = cdr(args);  sx_t y = car(args);
        if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
        origin = y->u.intval;
    }
    intval_t step = 1;
    if (ARGC == 3) {
        args = cdr(args);  sx_t z = car(args);
        if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
        step = z->u.intval;
    }

    sx_t *work = eval_alloc(1);

    sx_t *dst = DST;
    sx_assign_nil(dst);
    for (; cnt > 0; --cnt, origin += step) {
        list_append(&dst, int_new(&work[-1], origin));
    }
}


static void cf_list_concat(void)
{
    sx_assign(DST, cdr(ARGS));
}


static void _cf_list_toiter(sx_t sx)
{
    int n = list_len(sx);
    if (n < 0)  except_bad_arg(sx);
    if (n >= 1 && car(sx) == main_consts.function) {
        sx_assign(DST, cadr(sx));

        return;
    }
    
    iter_new(DST, SX_TYPE_LISTITER, sx, 0, sx);
}


static void cf_list_toiter(void)
{
    cf_argc_chk(1);
    _cf_list_toiter(car(ARGS));
}


static void cf_listiter_new(void)
{
    cf_argc_chk(2);
    _cf_list_toiter(cadr(ARGS));
}


static void cf_listiter_next(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_LISTITER)  except_bad_arg(x);

    struct sx_iterval *i = x->u.iterval;
    sx_t y = i->li;
    if (y == 0) {
        sx_assign_nil(DST);

        return;
    }

    cons(DST, car(y), 0);
    i->li = cdr(y);
}


static void cf_listiter_reset(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_LISTITER)  except_bad_arg(x);

    struct sx_iterval *i = x->u.iterval;
    i->li = i->sx;

    sx_assign(DST, x);
}


static void _cf_barray_toiter(sx_t sx)
{
    if (sx_type(sx) != SX_TYPE_BARRAY)  except_bad_arg(sx);

    iter_new(DST, SX_TYPE_BARRAYITER, sx, 0, 0);    
}


static void cf_barray_toiter(void)
{
    cf_argc_chk(1);
    _cf_barray_toiter(car(ARGS));
}


static void cf_barrayiter_new(void)
{
    cf_argc_chk(2);
    _cf_barray_toiter(cadr(ARGS));
}


static void cf_barrayiter_reset(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAYITER)  except_bad_arg(x);

    x->u.iterval->idx = 0;

    sx_assign(DST, x);
}


static void cf_barrayiter_prev(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAYITER)  except_bad_arg(x);

    struct sx_iterval   *i = x->u.iterval;
    struct sx_barrayval *b = i->sx->u.barrayval;
    if (i->idx < 2) {
        i->idx = 0;

        sx_assign_nil(DST);

        return;
    }

    cons(DST, int_new(DST, b->data[i->idx - 2]), 0);
    --i->idx;
}


static void cf_barrayiter_next(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_BARRAYITER)  except_bad_arg(x);

    struct sx_iterval   *i = x->u.iterval;
    struct sx_barrayval *b = i->sx->u.barrayval;
    if (i->idx >= b->size) {
        sx_assign_nil(DST);

        return;
    }

    cons(DST, int_new(DST, b->data[i->idx]), 0);
    ++i->idx;
}


static void _cf_array_toiter(sx_t sx)
{
    if (sx_type(sx) != SX_TYPE_ARRAY)  except_bad_arg(sx);

    iter_new(DST, SX_TYPE_ARRAYITER, sx, 0, 0);    
}


static void cf_array_toiter(void)
{
    cf_argc_chk(1);
    _cf_array_toiter(car(ARGS));
}


static void cf_arrayiter_new(void)
{
    cf_argc_chk(2);
    _cf_array_toiter(cadr(ARGS));
}


static void cf_arrayiter_reset(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAYITER)  except_bad_arg(x);

    x->u.iterval->idx = 0;

    sx_assign(DST, x);
}


static void cf_arrayiter_prev(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAYITER)  except_bad_arg(x);

    struct sx_iterval  *i = x->u.iterval;
    struct sx_arrayval *a = i->sx->u.arrayval;
    if (i->idx < 2) {
        i->idx = 0;

        sx_assign_nil(DST);

        return;
    }

    cons(DST, a->data[i->idx - 2], 0);
    --i->idx;
}


static void cf_arrayiter_next(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_ARRAYITER)  except_bad_arg(x);

    struct sx_iterval  *i = x->u.iterval;
    struct sx_arrayval *a = i->sx->u.arrayval;
    if (i->idx >= a->size) {
        sx_assign_nil(DST);

        return;
    }

    cons(DST, a->data[i->idx], 0);
    ++i->idx;
}


static void _cf_set_toiter(sx_t sx)
{
    if (!sx_is_set(sx))  except_bad_arg(sx);
    if (!sx->flags.frozen)  except_not_frozen(sx);
    struct sx_arrayval *a = sx->u.setval->base;
    sx_t     *p, li;
    unsigned i;
    for (p = a->data, i = 0; i < a->size; ++p, ++i) {
        li = *p;
        if (li != 0)  break;
    }
    
    iter_new(DST, SX_TYPE_SETITER, sx, i, li);
}


static void cf_set_toiter(void)
{
    cf_argc_chk(1);
    _cf_set_toiter(car(ARGS));
}


static void cf_setiter_new(void)
{
    cf_argc_chk(2);
    _cf_set_toiter(cadr(ARGS));
}


static void _cf_closure_togen(sx_t sx)
{
    if (sx_type(sx) != SX_TYPE_CLOSURE)  except_bad_arg(sx);

    closuregen_new(DST, sx);
}


static void cf_closure_togen(void)
{
    cf_argc_chk(1);
    _cf_closure_togen(car(ARGS));
}


static void cf_closuregen_new(void)
{
    cf_argc_chk(2);
    _cf_closure_togen(cadr(ARGS));
}


static void cf_closuregen_next(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSUREGEN)  except_bad_arg(x);
    struct sx_closuregen *g = x->u.closuregenval;

    sx_t *work = eval_alloc(1);

    cons(&work[-1], g->cl, 0);
    method_call_apply(DST, main_consts.calln, 1, work[-1], ARGS_EVAL_SUPPRESS_ALL);    
}


static void cf_closuregen_reset(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLOSUREGEN)  except_bad_arg(x);
    struct sx_closuregen *g = x->u.closuregenval;
    set_copydeep_unsafe(&g->cl->u.closureval->dict, g->dict);

    sx_assign(DST, x);
}


static void cf_setiter_next(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_SETITER)  except_bad_arg(x);

    struct sx_iterval  *i = x->u.iterval;
    struct sx_arrayval *a = i->sx->u.setval->base;
    if (i->idx >= a->size) {
        sx_assign_nil(DST);

        return;
    }

    sx_t li = i->li;
    cons(DST, car(li), 0);
    i->li = cdr(li);
    if (i->li != 0)  return;

    sx_t *p;
    for (p = &a->data[++i->idx]; i->idx < a->size; ++p, ++i->idx) {
        li = *p;
        if (li != 0)  break;
    }
    i->li = li;
}


static void cf_setiter_reset(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_SETITER)  except_bad_arg(x);

    struct sx_iterval  *i = x->u.iterval;
    
    struct sx_arrayval *a = i->sx->u.setval->base;
    sx_t     *p, li;
    unsigned k;
    for (p = a->data, k = 0; k < a->size; ++p, ++k) {
        li = *p;
        if (li != 0)  break;
    }
    
    i->idx = k;
    i->li  = li;

    sx_assign(DST, x);
}


void _cf_closure_togenerator(sx_t sx)
{
    if (sx_type(sx) != SX_TYPE_CLOSURE)  except_bad_arg(sx);
    
    
}


static void cf_dptr_repr(void)
{
    if (ARGC != 1)  except_num_args(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(x);

    unsigned size = 3;          /* "(" + ")" + terminator */

    sx_t *work = eval_alloc(2);

    sx_t *p = &work[-1];
    bool f = false;
    bool dpf = false;
    sx_t y;
    for (; x != 0; x = cdr(x), f = true) {
        if (f)  ++size;     /* " " */
        if (sx_type(x) != SX_TYPE_DPTR)  dpf = true;
        if (dpf) {
            size += 2;          /* ". " */
            y = x;
        } else {
            y = car(x);
        }
        struct sx_strval *s = sx_repr(&work[-2], y);
        list_append(&p, work[-2]);
        size += s->size - 1;
        if (dpf)  break;
    }

    char *buf = mem_alloc(size);
    FILE *fp = fmemopen(buf, size, "w");
    fprintf(fp, "(");
    f = false;
    for (x = work[-1]; x != 0; x = y, f = true) {
        y = cdr(x);
        if (f)  fprintf(fp, " ");
        fprintf(fp, "%s", car(x)->u.strval->data);
        if (y != 0 && cdr(y) == 0 && dpf)  fprintf(fp, " .");
    }
    fprintf(fp, ")");
    fclose(fp);

    str_newm(DST, size, buf);
}


static void cf_dptr_tolist(void)
{
    if (ARGC != 1)  except_num_args(1);
    sx_t x = car(ARGS);
    if (list_len(x) < 0)  except_bad_arg(x);

    sx_assign(DST, x);
}


static void cf_dptr_eval(void)  /* nsubr */
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);

#if 0
    if (debug_tracef) {
        debug_indent();
        fputs(">>> ", stderr);
        sx_print(stderr, x);
        fputs("\n", stderr);
        ++debug_lvl;
    }
#endif
    
    sx_t *work = eval_alloc(1);

    sx_t f = car(x), args = cdr(x);
    unsigned argc = list_len(args);
    if (sx_type(f) == SX_TYPE_SYM) {
        if (f == main_consts.lambda
            || f == main_consts.nlambda
            || f == main_consts.macro
            ) {
            sx_assign(DST, x);

            return;
        }
        
        if (!_env_find_from(0, &f, f, ENVFP)) {
            /* No named function defined, call as method */

            if (args == 0)  except_symbol_not_bound(f);

            /* Compute method receiver */

            struct sx_strval *s = f->u.strval;
            if (s->size >= 2 && s->data[0] == '-') {
              sx_assign(&work[-1], car(args));
            } else {
              sx_eval(&work[-1], car(args));
            }

            eval_alloc(1);

            method_find_except(&work[-2], work[-1], f);
            cons(&work[-1], work[-2], cons(&work[-1], work[-1], cdr(args)));
            method_call_apply(DST, main_consts.call1, 1 + argc, work[-1], 1);

            return;
        }
    } else {
        sx_eval(&work[-1], f);
        f = work[-1];
    }

    cons(&work[-1], f, args);
    method_call_apply(DST, main_consts.call, 1 + argc, work[-1], 0);

#if 0
    if (debug_tracef) {
        --debug_lvl;
        debug_indent();
        fputs("<<< ", stderr);
        sx_print(stderr, *DST);
        fputs("\n", stderr);
    }
#endif
}


static void cf_dptr_equal(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(x);

    bool result = false;
    do {
        sx_t y = cadr(ARGS);
        if (sx_type(y) != SX_TYPE_DPTR)  break;
        for (; ; x = cdr(x), y = cdr(y)) {
            if (x == 0 || y == 0) {
                result = (x == 0 && y == 0);
                
                break;
            }
            
            if (sx_type(x) == SX_TYPE_DPTR) {
                if (sx_type(y) == SX_TYPE_DPTR) {
                    if (sx_equal(car(x), car(y)))  continue;
                }

                break;
            }

            result = sx_equal(x, y);
            
            break;
        }
        
    } while (0);
    
    bool_new(DST, result);
}


static void cf_dptr_hash(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(x);

    intval_t h = 0;
    for (; x != 0; x = cdr(x)) {
        if (sx_type(x) != SX_TYPE_DPTR) {
            h += sx_hash(x);

            break;
        }

        h += sx_hash(car(x));
    }

    int_new(DST, h);
}


static void cf_dptr_apply(void)
{
#if 0
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (list_len(y) < 0)  except_bad_arg(y);

    sx_apply(DST, 0, x, y, 0);
#endif
}


static void cf_dptr_applya(void)
{
#if 0
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_DPTR)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (list_len(y) < 0)  except_bad_arg(y);

    sx_apply(DST, 0, x, y, -1);
#endif
}


static void cf_dptr_append(void)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (list_len(x) < 0)  except_bad_arg(x);

    cons(list_copy_unsafe(DST, x), cadr(ARGS), 0);
}


static void cf_rep(void)
{
    //    rep();

    sx_assign_nil(DST);
}


static void cf_obj_instof(void)
{
    cf_argc_chk(1);
    sx_assign(DST, sx_inst_of(car(ARGS)));
}


static void cf_obj_method_find(void)
{
    cf_argc_chk(2);
    sx_t sel = cadr(ARGS);
    if (sx_type(sel) != SX_TYPE_SYM)  except_bad_arg(sel);
    
    if (method_find(DST, car(ARGS), sel))  return;
    sx_assign_nil(DST);
}


static void cf_method_find(void)
{
    cf_argc_chk_range(3, 4);
    sx_t args = ARGS;
    args = cdr(args);  sx_t x = car(args);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_SYM)  except_bad_arg(y);
    sx_t z;
    if (ARGC == 4) {
        args = cdr(args);  z = car(args);
        if (sx_type(z) != SX_TYPE_CLASS)  except_bad_arg(z);
    } else {
        z = sx_inst_of(x);
    }

    if ((sx_type(x) == SX_TYPE_CLASS && method_find_type(DST, y, ARGC == 4 ? z : x, OFS_CLASS_METHODS))
        || method_find_type(DST, y, z, OFS_INST_METHODS)
        ) {
        return;
    }

    sx_assign_nil(DST);
}


static void cf_method_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_METHOD)  except_bad_arg(x);
    struct sx_methodval *m = x->u.methodval;
    struct sx_strval *s1 = sx_inst_of(x)->u.classval->class->name->u.strval;

    sx_t *work = eval_alloc(2);
    
    struct sx_strval *s2 = sx_repr(&work[-1], m->class);
    struct sx_strval *s3 = sx_repr(&work[-2], m->func);
    unsigned bufsize = 1 + s1->size - 1 + 2 + s2->size - 1 + 1 + s3->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
    snprintf(buf, bufsize, "<%s: %s %s>", s1->data, s2->data, s3->data);
    str_newm(DST, bufsize, buf);
}


static void cf_metaclass_new(void)
{
    cf_argc_chk(3);
    sx_t args = cdr(ARGS);
    sx_t nm = car(args);
    if (sx_type(nm) != SX_TYPE_SYM)  assert(0);
    args = cdr(args);
    sx_t parent = car(args);
    if (sx_type(parent) != SX_TYPE_CLASS)  assert(0);

    env_bind(nm, class_new(DST, nm, parent));
}


static void cf_class_current(void)
{
    cf_argc_chk(1);
    struct frame_class *p = CLASSFP;
    unsigned n;
    for (n = 11; p != 0 && n > 0; --n)  p = (struct frame_class *) p->base->type_prev;
    sx_assign(DST, p == 0 ? 0 : p->class);
}


static void cf_class_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLASS)  except_bad_arg(x);

    struct sx_classval *cl = x->u.classval;
    struct sx_strval *s = cl->class->name->u.strval;
    static const char ldr[] = "<main.Metaclass: ";
    static const char trlr[] = ">";
    unsigned bufsize = sizeof(ldr) - 1 + s->size - 1 + sizeof(trlr) - 1 + 1;
    char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
    snprintf(buf, bufsize, "%s%s%s", ldr, s->data, trlr);
    str_newm(DST, bufsize, buf);
}


static bool _cf_class_at(sx_t *key, sx_t *value)
{
    cf_argc_chk(2);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLASS)  except_bad_arg(x);
    sx_t y = cadr(ARGS);
    if (sx_type(y) != SX_TYPE_SYM)  except_bad_arg(y);
    struct class *cl = x->u.classval->class;
    struct sx_strval *s = y->u.strval;

    *key = y;
    if (str_equalc(s, STR_CONST("name"))) {
        *value = cl->name;

        return (true);
    }
    if (str_equalc(s, STR_CONST("parent"))) {
        *value = cl->parent;

        return (true);
    }
    if (str_equalc(s, STR_CONST("class-variables"))) {
        *value = cl->class_vars;

        return (true);
    }
    if (str_equalc(s, STR_CONST("class-methods"))) {
        *value = cl->class_methods;

        return (true);
    }
    if (str_equalc(s, STR_CONST("instance-methods"))) {
        *value = cl->inst_methods;

        return (true);
    }

    return (false);
}


static void cf_class_at(void)
{
    sx_t k, v;
    if (_cf_class_at(&k, &v)) {
        cons(DST, k, v);

        return;
    }

    sx_assign_nil(DST);
}


static void cf_class_ate(void)
{
    sx_t k, v;
    if (_cf_class_at(&k, &v)) {
        sx_assign(DST, v);

        return;
    }

    except_no_member(car(ARGS), k);
}


static void cf_class_instof(void)
{
    cf_argc_chk(1);
    sx_t x = car(ARGS);
    if (sx_type(x) != SX_TYPE_CLASS)  except_bad_arg(x);

    sx_assign(DST, main_consts.Metaclass);
}


static void cf_obj_new(void)    /* Nsubr */
{
    cf_argc_chk_min(1);
    sx_t cl = car(ARGS);

    sx_t *work = eval_alloc(3);

    obj_new(DST, cl);           /* See cf note 1 */

    if (!method_find_type(&work[-1], main_consts.__init__, cl, OFS_INST_METHODS))  return;
    
    cons(&work[-2], work[-1], cons(&work[-2], *DST, cdr(ARGS)));
    method_call_apply(&work[-3], main_consts.call1, 1 + ARGC, work[-2], 1);
}


static sx_t _cf_obj_at(void)
{
    cf_argc_chk(2);
    sx_t recvr = car(ARGS);
    if (sx_type(recvr) != SX_TYPE_OBJ)  except_bad_arg(recvr);
    sx_t k = cadr(ARGS);
    if (sx_type(k) != SX_TYPE_SYM)  except_bad_arg(k);

    return (dict_ats(recvr->u.objval->dict->u.setval, k, sx_hash(k)));
}


static void cf_obj_at(void)
{
    sx_assign(DST, _cf_obj_at());
}


static void cf_obj_ate(void)
{
    sx_t pr = _cf_obj_at();
    if (pr == 0)  except_no_member(car(ARGS), cadr(ARGS));
    
    sx_assign(DST, cdr(pr));
}

 
static void cf_obj_atput(void)
{
    cf_argc_chk(3);
    sx_t args = ARGS;
    sx_t recvr = car(args);
    if (sx_type(recvr) != SX_TYPE_OBJ)  except_bad_arg(recvr);
    args = cdr(args);  sx_t k = car(args);
    if (sx_type(k) != SX_TYPE_SYM)  except_bad_arg(k);
    if (recvr->flags.frozen)  except_frozen(recvr);
    args = cdr(args);  sx_t val = car(args);

    dict_atsput(recvr->u.objval->dict->u.setval, k, sx_hash(k), val);

    sx_assign(DST, val);
}


static inline intval_t _obj_cmp(void)
{
    cf_argc_chk(2);
    return (sx_cmp(car(ARGS), cadr(ARGS)));
}


static void cf_obj_lt(void)
{
    bool_new(DST, _obj_cmp() < 0);
}

 
static void cf_obj_le(void)
{
    bool_new(DST, _obj_cmp() <= 0);
}

 
static void cf_obj_ge(void)
{
    bool_new(DST, _obj_cmp() >= 0);
}

 
static void cf_obj_gt(void)
{
    bool_new(DST, _obj_cmp() > 0);
}

 
void except_assert_failed(sx_t sx, sx_t mesg)
{
    except_raise1();
    
    except_newl(STR_CONST("system.assert-failed"), 1, sx);

    frame_except_raise();
}


static void cf_assert(void)
{
    cf_argc_chk_range(1, 2);
    sx_t x = car(ARGS);

    sx_t *work = eval_alloc(1);

    sx_eval(&work[-1], x);
    if (work[-1] == 0) {
        except_assert_failed(x, ARGC == 2 ? cadr(ARGS) : 0);
    }

    sx_assign(DST, main_consts.t);
}

#ifndef NDEBUG

static void cf_debug(void)
{
    debug_rep();
    
    sx_assign_nil(DST);
}


static void cf_trace(void)
{
    debug_tracef = (ARGC >= 1 && car(ARGS) != 0);
    
    bool_new(DST, debug_tracef);
}


static void cf_collect(void)
{
    collect();
    sx_assign_nil(DST);
}

/***************************************************************************
 *
 * Initialization
 *
 ***************************************************************************/

#endif  /* NDEBUG */

static void stack_init(unsigned eval_stack_size, unsigned frame_stack_size)
{
    unsigned sys_page_size = getpagesize();

    unsigned eval_stack_size_bytes = eval_stack_size * sizeof(vm->eval_stack[0]);
    if (eval_stack_size_bytes < sys_page_size) {
        eval_stack_size_bytes = sys_page_size;
    } else {
        eval_stack_size_bytes = round_up_to_power_of_2(eval_stack_size_bytes);
    }
    vm->eval_stack = (sx_t *) mem_pages_alloc(eval_stack_size_bytes);
    vm->eval_stack_top = vm->sp = vm->eval_stack + eval_stack_size;

    if (frame_stack_size < sys_page_size) {
        frame_stack_size = sys_page_size;
    } else {
        frame_stack_size = round_up_to_power_of_2(frame_stack_size);
    }
    vm->frame_stack = (unsigned char *) mem_pages_alloc(frame_stack_size);
    vm->frame_stack_top = vm->frame_stack + frame_stack_size;
    vm->fp = (struct frame *) vm->frame_stack_top;
}


void consts_init(unsigned n, const struct const_init *init)
{
    sx_t *work = eval_alloc(2);

    for (; n > 0; --n, ++init) {
        sym_newb(&work[-1], init->name_size, init->name);
        switch (init->type) {
        case SX_TYPE_INT:
            int_new(&work[-2], init->u.intval);
            break;
        case SX_TYPE_FLOAT:
            float_new(&work[-2], init->u.floatval);
            break;
        case SX_TYPE_SYM:
            sym_newb(&work[-2], init->u.strval->size, init->u.strval->data);
            break;
        case SX_TYPE_STR:
            str_newb(&work[-2], init->u.strval->size, init->u.strval->data);
            break;
        default:
            assert(0);
        }

        env_bind(work[-1], work[-2]);
    }

    eval_unwind(work);
}


void funcs_init(unsigned modname_size, const char *modname, unsigned n, const struct func_init *init)
{
    sx_t *work = eval_alloc(3);
    
    for (; n > 0; --n, ++init) {
        sym_newb(&work[-1], init->sym_size, init->sym_data); /* [-1] := Symbol */
        unsigned bufsize = init->sym_size - 1 + 1 + modname_size - 1  + 1;
        char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
        snprintf(buf, bufsize, "%s.%s", modname, init->sym_data);
        sym_newm(&work[-2], bufsize, buf); /* [-2] := Printable func name */
        (*init->constructor)(&work[-3], init->func, work[-2]); /* [-3] := Callable */
        env_bind(work[-1], work[-3]);
    }

    eval_unwind(work);
}


static const struct func_init init_tbl[] = {
    { STR_CONST("lambda"), nsubr_new, cf_lambda },
    { STR_CONST("nlambda"), nsubr_new, cf_nlambda },
    { STR_CONST("macro"), nsubr_new, cf_macro },
    { STR_CONST("function"), nsubr_new, cf_function },
    { STR_CONST("def"), nsubr_new, cf_def },
    { STR_CONST("setq"), nsubr_new, cf_setq },
    { STR_CONST("setq^"), nsubr_new, cf_setq_up },
    { STR_CONST("set"), subr_new, cf_set },
    { STR_CONST("unsetq"), nsubr_new, cf_unsetq },
    { STR_CONST("boundp"), nsubr_new, cf_boundp },
    { STR_CONST("pass"), subr_new, cf_pass },
    { STR_CONST("eval"), subr_new, cf_eval },
    { STR_CONST("funccall"), subr_new,  cf_funccall },
    { STR_CONST("apply"), subr_new,  cf_apply },
    { STR_CONST("method-funccall"), subr_new,  cf_method_funccall },
    { STR_CONST("method-apply"), subr_new,  cf_method_apply },
    { STR_CONST("try"),      nsubr_new, cf_try },
    { STR_CONST("raise"),    subr_new,  cf_raise },
    { STR_CONST("print"), subr_new, cf_print },
    { STR_CONST("prog"), nsubr_new, cf_prog },
    { STR_CONST("progn"), nsubr_new, cf_progn },
    { STR_CONST("progn^"), nsubr_new, cf_progn_up },
    { STR_CONST("let"), nsubr_new, cf_let },
    { STR_CONST("let*"),     nsubr_new, cf_leta },
    { STR_CONST("goto"),     nsubr_new, cf_goto },
    { STR_CONST("return"),   subr_new,  cf_return },
    { STR_CONST("cond"),     nsubr_new, cf_cond },
    { STR_CONST("while"),    nsubr_new, cf_while },
    { STR_CONST("break"),    nsubr_new, cf_break },
    { STR_CONST("continue"), nsubr_new, cf_continue },
    { STR_CONST("if"),       nsubr_new, cf_if },
    { STR_CONST("eq"),       subr_new,  cf_eq },
    { STR_CONST("not"),      subr_new,  cf_not },
    { STR_CONST("and"),      nsubr_new, cf_and },
    { STR_CONST("or"),       nsubr_new, cf_or },
    { STR_CONST("mapcar"),   subr_new,  cf_mapcar },
    { STR_CONST("outer"),    subr_new,  cf_outer },
    { STR_CONST("filter"),   subr_new,  cf_filter },
    { STR_CONST("filter*"),  subr_new,  cf_filter_alt },
    { STR_CONST("foreach"),  subr_new,  cf_foreach },
    { STR_CONST("exit"),     subr_new,  cf_exit },
    { STR_CONST("rep"),      subr_new,  cf_rep },
    { STR_CONST("assert"),   nsubr_new, cf_assert }
#ifndef NDEBUG
    ,
    { STR_CONST("debug"),    subr_new,  cf_debug },
    { STR_CONST("trace"),    subr_new,  cf_trace },
    { STR_CONST("collect"),  subr_new,  cf_collect },
#endif  /* NDEBUG */
};

static void class_file_init(sx_t cl)
{
    struct sx_setval *d = cl->u.classval->class->class_vars->u.setval;

    sx_t *work = eval_alloc(3);

    sym_newb(&work[-1], STR_CONST("stdin"));
    sym_newb(&work[-2], STR_CONST("r"));
    file_new(&work[-3], work[-1], work[-2], stdin);
    dict_atsput(d, work[-1], sx_hash(work[-1]), work[-3]);
    sym_newb(&work[-1], STR_CONST("stdout"));
    sym_newb(&work[-2], STR_CONST("w"));
    file_new(&work[-3], work[-1], work[-2], stdout);
    dict_atsput(d, work[-1], sx_hash(work[-1]), work[-3]);
    sym_newb(&work[-1], STR_CONST("stderr"));
    sym_newb(&work[-2], STR_CONST("w"));
    file_new(&work[-3], work[-1], work[-2], stderr);
    dict_atsput(d, work[-1], sx_hash(work[-1]), work[-3]);

    eval_unwind(work);
}


static void class_set_init(sx_t cl)
{
    sx_t *work = eval_alloc(1);

    int_new(&work[-1], 32);
    dict_atsput(cl->u.classval->class->class_vars->u.setval,
                main_consts.default_size,
                sx_hash(main_consts.default_size),
                work[-1]
                );

    eval_unwind(work);
}


static void class_module_init(sx_t cl)
{
    sx_t *work = eval_alloc(2);

    char *e = getenv("LL6MODULEPATH");
    if (e != 0) {
        sx_t *p = &work[-1];

        while (*e != 0) {
            char *f = index(e, ':');
            str_newc(&work[-2], (f == 0 ? strlen(e) : f - e) + 1, e);
            list_append(&p, work[-2]);
            if (f == 0)  break;
            e = f + 1;
        }
    }
    
    dict_atsput(cl->u.classval->class->class_vars->u.setval,
                main_consts.path,
                sx_hash(main_consts.path),
                work[-1]
                );

    dict_new(&work[-1], 32);
    
    dict_atsput(cl->u.classval->class->class_vars->u.setval,
                main_consts.__loaded__,
                sx_hash(main_consts.__loaded__),
                work[-1]
                );    
    
    eval_unwind(work);    
}

static const struct class_init class_init_tbl[] = {
    { &main_consts.Metaclass, STR_CONST("Metaclass"), 0 },
    { &main_consts.Obj,       STR_CONST("Obj"),       0 },
    { &main_consts.Int,       STR_CONST("Int"),       &main_consts.Obj },
    { &main_consts.Float,     STR_CONST("Float"),     &main_consts.Obj },
    { &main_consts.Complex,   STR_CONST("Complex"),   &main_consts.Obj },
    { &main_consts.String,    STR_CONST("String"),    &main_consts.Obj },
    { &main_consts.Symbol,    STR_CONST("Symbol"),    &main_consts.String },
    { &main_consts.Nsubr,     STR_CONST("Nsubr"),     &main_consts.Obj },
    { &main_consts.Subr,      STR_CONST("Subr"),      &main_consts.Obj },
    { &main_consts.File,      STR_CONST("File"),      &main_consts.Obj, class_file_init },
    { &main_consts.Barray,    STR_CONST("Barray"),    &main_consts.Obj },
    { &main_consts.Array,     STR_CONST("Array"),     &main_consts.Obj },
    { &main_consts.Earray,    STR_CONST("Earray"),    &main_consts.Obj },
    { &main_consts.Set,       STR_CONST("Set"),       &main_consts.Obj, class_set_init },
    { &main_consts.Dict,      STR_CONST("Dict"),      &main_consts.Set, class_set_init },
    { &main_consts.Closure,   STR_CONST("Closure"),   &main_consts.Obj },
    { &main_consts.Env,       STR_CONST("Env"),       &main_consts.Obj },
    { &main_consts.Module,    STR_CONST("Module"),    &main_consts.Obj, class_module_init },
    { &main_consts.Dptr,      STR_CONST("Dptr"),      &main_consts.Obj },
    { &main_consts.List,      STR_CONST("List"),      &main_consts.Dptr },
    { &main_consts.Method,    STR_CONST("Method"),    &main_consts.Obj },
    { &main_consts.Generator, STR_CONST("Generator"), &main_consts.Obj },
    { &main_consts.ListIter,  STR_CONST("ListIter"),  &main_consts.Generator },
    { &main_consts.BarrayIter, STR_CONST("BarrayIter"), &main_consts.Generator },
    { &main_consts.ArrayIter, STR_CONST("ArrayIter"), &main_consts.Generator },
    { &main_consts.SetIter,   STR_CONST("SetIter"),   &main_consts.Generator },
    { &main_consts.ClosureGenerator, STR_CONST("ClosureGenerator"), &main_consts.Generator }
};

static void _classes_init(void)
{
    sx_t *work = eval_alloc(2);

    sym_newb(&work[-2], STR_CONST("main.Metaclass"));
    class_new_init(&work[-1], 0, work[-2], 0);
    sym_newb(&work[-2], STR_CONST("Metaclass"));
    env_bind(work[-2], work[-1]);
    main_consts.Metaclass = work[-1];
    
    unsigned i;
    for (i = 1; i < ARRAY_SIZE(class_init_tbl); ++i) {
        unsigned bufsize = 4 + 1 + class_init_tbl[i].name_size - 1 + 1;
        char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
        snprintf(buf, bufsize, "main.%s", class_init_tbl[i].name_data);
        sym_newm(&work[-2], bufsize, buf);
        sx_t *parent = class_init_tbl[i].parent;
        class_new(&work[-1], work[-2], parent == 0 ? 0 : *parent);
        sym_newb(&work[-2], class_init_tbl[i].name_size, class_init_tbl[i].name_data);
        env_bind(work[-2], work[-1]);
        *class_init_tbl[i].dst = work[-1];
    }
    sx_assign(&main_consts.Metaclass->u.classval->inst_of, main_consts.Obj);

    eval_unwind(work);
}


static void _classes_init2(void)
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(class_init_tbl); ++i) {
        void (*f)(sx_t) = class_init_tbl[i].init;
        if (f != 0)  (*f)(*class_init_tbl[i].dst);
    }
}


void classes_init(unsigned modname_size, const char *modname, unsigned n, const struct class_init *init)
{
    sx_t *work = eval_alloc(2);

    unsigned i;
    for (i = 0; i < n; ++i) {
        unsigned bufsize = modname_size - 1 + 1 + init[i].name_size - 1 + 1;
        char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
        snprintf(buf, bufsize, "%s.%s", modname, init[i].name_data);
        sym_newm(&work[-2], bufsize, buf);
        sx_t *parent = init[i].parent;
        class_new(&work[-1], work[-2], parent == 0 ? 0 : *parent);
        sym_newb(&work[-2], init[i].name_size, init[i].name_data);
        env_bind(work[-2], work[-1]);
        *init[i].dst = work[-1];
    }

    for (i = 0; i < n; ++i) {
        void (*f)(sx_t) = init[i].init;
        if (f != 0)  (*f)(*init[i].dst);
    }

    eval_unwind(work);
}

static const struct method_init method_init_tbl[] = {
    { &main_consts.Metaclass, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_metaclass_new },
    { &main_consts.Metaclass, OFS_CLASS_METHODS, STR_CONST("repr"), subr_new, cf_class_repr },
    { &main_consts.Metaclass, OFS_CLASS_METHODS, STR_CONST("tostring"), subr_new, cf_class_repr },
    { &main_consts.Metaclass, OFS_CLASS_METHODS, STR_CONST("at"), subr_new, cf_class_at },
    { &main_consts.Metaclass, OFS_CLASS_METHODS, STR_CONST("ate"), subr_new, cf_class_ate },
    { &main_consts.Metaclass, OFS_CLASS_METHODS, STR_CONST("current"), subr_new, cf_class_current },
    { &main_consts.Metaclass, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_class_repr },
    { &main_consts.Metaclass, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_class_repr },
    { &main_consts.Metaclass, OFS_INST_METHODS,  STR_CONST("at"), subr_new, cf_class_at },
    { &main_consts.Metaclass, OFS_INST_METHODS,  STR_CONST("ate"), subr_new, cf_class_ate },
    { &main_consts.Metaclass, OFS_INST_METHODS,  STR_CONST("instance-of"), subr_new, cf_class_instof },

    { &main_consts.Obj, OFS_CLASS_METHODS, STR_CONST("new"), nsubr_new, cf_obj_new },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("copy"), subr_new, cf_obj_copy },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("copydeep"), subr_new, cf_obj_copydeep },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("eval"), nsubr_new, cf_obj_eval },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_obj_repr },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_obj_repr },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("tolist"), subr_new, cf_obj_tolist },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("instance-of"), subr_new, cf_obj_instof },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("method-find"), subr_new, cf_obj_method_find },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("at"), subr_new, cf_obj_at },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("ate"), subr_new, cf_obj_ate },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("atput"), subr_new, cf_obj_atput },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("atomp"), subr_new, cf_true },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("equal"), nsubr_new, cf_obj_equal },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("hash"), subr_new, cf_obj_hash },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("lt"), subr_new, cf_obj_lt },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("le"), subr_new, cf_obj_le },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("ge"), subr_new, cf_obj_ge },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("gt"), subr_new, cf_obj_gt },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("append"),  subr_new,  cf_obj_append },
    { &main_consts.Obj, OFS_INST_METHODS,  STR_CONST("reduce"), subr_new, cf_reduce_tolist },

    { &main_consts.Int, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_int_new },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_int_repr },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_int_repr },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("cmp"), subr_new, cf_int_cmp },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("equal"), subr_new, cf_int_equal },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("hash"), subr_new, cf_int_hash },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("octal"), subr_new, cf_int_octal },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("hex"), subr_new, cf_int_hex },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("abs"), subr_new, cf_int_abs },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("minus"), subr_new, cf_int_minus },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("re"), subr_new, cf_int_re },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("im"), subr_new, cf_int_im },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("add"), subr_new, cf_int_add },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("sub"), subr_new, cf_int_sub },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("mul"), subr_new, cf_int_mul },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("div"), subr_new, cf_int_div },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("mod"), subr_new, cf_int_mod },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("lsh"), subr_new, cf_int_lsh },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("rsh"), subr_new, cf_int_rsh },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("ursh"), subr_new, cf_int_ursh },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("bitand"), subr_new, cf_int_bitand },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("bitor"), subr_new, cf_int_bitor },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("bitnot"), subr_new, cf_int_bitnot },
    { &main_consts.Int, OFS_INST_METHODS,  STR_CONST("iota"),   subr_new, cf_int_iota },

    { &main_consts.Float, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_float_new },
    { &main_consts.Float, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_float_repr },
    { &main_consts.Float, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_float_repr },
    { &main_consts.Float, OFS_INST_METHODS,  STR_CONST("hash"), subr_new, cf_float_hash },
    { &main_consts.Float, OFS_INST_METHODS,  STR_CONST("equal"), subr_new, cf_float_equal },
    { &main_consts.Float, OFS_INST_METHODS,  STR_CONST("add"), subr_new, cf_float_add },
    { &main_consts.Float, OFS_INST_METHODS,  STR_CONST("sub"), subr_new, cf_float_sub },
    { &main_consts.Float, OFS_INST_METHODS,  STR_CONST("mul"), subr_new, cf_float_mul },

    { &main_consts.Complex, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_complex_new },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_complex_repr },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_complex_repr },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("hash"), subr_new, cf_complex_hash },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("abs"), subr_new, cf_complex_abs },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("arg"), subr_new, cf_complex_arg },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("re"), subr_new, cf_complex_re },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("im"), subr_new, cf_complex_im },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("add"), subr_new, cf_complex_add },
    { &main_consts.Complex, OFS_INST_METHODS,  STR_CONST("sub"), subr_new, cf_complex_sub },

    { &main_consts.String, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_str_new },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_str_repr },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_str_tostring },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("tonumber"), subr_new, cf_str_tonumber },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("read"), subr_new, cf_str_read },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("cmp"), subr_new, cf_str_cmp },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("hash"), subr_new, cf_str_hash },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("equal"), subr_new, cf_str_equal },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("split"), subr_new, cf_str_split },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("index"), subr_new, cf_str_index },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("rindex"), subr_new, cf_str_rindex },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("format"), subr_new, cf_str_format },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("at"), subr_new, cf_str_at },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("slice"), subr_new, cf_str_slice },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("size"), subr_new, cf_str_size },
    { &main_consts.String, OFS_INST_METHODS,  STR_CONST("join"), subr_new, cf_str_join },

    { &main_consts.Symbol, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_sym_new },
    { &main_consts.Symbol, OFS_INST_METHODS,  STR_CONST("eval"), nsubr_new, cf_sym_eval },
    { &main_consts.Symbol, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_sym_repr },
    { &main_consts.Symbol, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_sym_repr },
    { &main_consts.Symbol, OFS_INST_METHODS,  STR_CONST("equal"), subr_new, cf_sym_equal },

    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_subr_repr },
    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_subr_repr },
    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("call"), nsubr_new, cf_nsubr_call },
    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("call1"), nsubr_new, cf_nsubr_call },
    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("calln"), nsubr_new, cf_nsubr_call },
    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("reduce"), subr_new, cf_reduce_callable },
    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("togenerator"), subr_new, cf_pass },
    { &main_consts.Nsubr, OFS_INST_METHODS,  STR_CONST("next"), subr_new, cf_call0 },

    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_subr_repr },
    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_subr_repr },
    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("call"), nsubr_new, cf_subr_call },
    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("call1"), nsubr_new, cf_subr_call1 },
    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("calln"), nsubr_new, cf_subr_calln },
    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("reduce"), subr_new, cf_reduce_callable },
    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("togenerator"), subr_new, cf_pass },
    { &main_consts.Subr, OFS_INST_METHODS,  STR_CONST("next"), subr_new, cf_call0 },

    { &main_consts.File, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_file_new },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("copy"), subr_new, cf_file_copy },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("copydeep"), subr_new, cf_file_copy },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_file_repr },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_file_repr },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("read"), subr_new, cf_file_read },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("readb"), subr_new, cf_file_readb },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("write"), subr_new, cf_file_write },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("lseek"), subr_new, cf_file_lseek },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("tell"), subr_new, cf_file_tell },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("fflush"), subr_new, cf_file_fflush },
    { &main_consts.File, OFS_INST_METHODS,  STR_CONST("close"), subr_new, cf_file_close },

    { &main_consts.Barray, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_barray_new },
    { &main_consts.Barray, OFS_INST_METHODS,  STR_CONST("copy"), subr_new, cf_barray_copy },
    { &main_consts.Barray, OFS_INST_METHODS,  STR_CONST("copydeep"), subr_new, cf_barray_copy },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("repr"), subr_new, cf_barray_repr },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("tostring"), subr_new, cf_barray_tostring },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("tolist"), subr_new, cf_barray_tolist },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("at"), subr_new, cf_barray_at },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("atput"), subr_new, cf_barray_atput },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("freeze"), subr_new, cf_freeze },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("frozenp"), subr_new, cf_frozenp },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("size"), subr_new, cf_barray_size },
    { &main_consts.Barray, OFS_INST_METHODS, STR_CONST("togenerator"), subr_new, cf_barray_toiter },

    { &main_consts.BarrayIter, OFS_CLASS_METHODS,  STR_CONST("new"), subr_new, cf_barrayiter_new },
    { &main_consts.BarrayIter, OFS_INST_METHODS,  STR_CONST("reset"), subr_new, cf_barrayiter_reset },
    { &main_consts.BarrayIter, OFS_INST_METHODS,  STR_CONST("prev"), subr_new, cf_barrayiter_prev },
    { &main_consts.BarrayIter, OFS_INST_METHODS,  STR_CONST("next"), subr_new, cf_barrayiter_next },

    { &main_consts.Array, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_array_new },
    { &main_consts.Array, OFS_INST_METHODS,  STR_CONST("copy"), subr_new, cf_array_copy },
    { &main_consts.Array, OFS_INST_METHODS,  STR_CONST("copydeep"), subr_new, cf_array_copydeep },
    { &main_consts.Array, OFS_INST_METHODS, STR_CONST("repr"), subr_new, cf_array_repr },
    { &main_consts.Array, OFS_INST_METHODS, STR_CONST("tostring"), subr_new, cf_array_repr },
    { &main_consts.Array, OFS_INST_METHODS, STR_CONST("tolist"), subr_new, cf_array_tolist },
    { &main_consts.Array, OFS_INST_METHODS,  STR_CONST("at"), subr_new, cf_array_at },
    { &main_consts.Array, OFS_INST_METHODS,  STR_CONST("atput"), subr_new, cf_array_atput },
    { &main_consts.Array, OFS_INST_METHODS,  STR_CONST("sort"), subr_new, cf_array_sort },
    { &main_consts.Array, OFS_INST_METHODS, STR_CONST("freeze"), subr_new, cf_freeze },
    { &main_consts.Array, OFS_INST_METHODS, STR_CONST("frozenp"), subr_new, cf_frozenp },
    { &main_consts.Array, OFS_INST_METHODS, STR_CONST("size"), subr_new, cf_array_size },
    { &main_consts.Array, OFS_INST_METHODS,  STR_CONST("togenerator"), subr_new, cf_array_toiter },

    { &main_consts.ArrayIter, OFS_CLASS_METHODS,  STR_CONST("new"), subr_new, cf_arrayiter_new },
    { &main_consts.ArrayIter, OFS_INST_METHODS,  STR_CONST("reset"), subr_new, cf_arrayiter_reset },
    { &main_consts.ArrayIter, OFS_INST_METHODS,  STR_CONST("prev"), subr_new, cf_arrayiter_prev },
    { &main_consts.ArrayIter, OFS_INST_METHODS,  STR_CONST("next"), subr_new, cf_arrayiter_next },

    { &main_consts.Set, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_set_new },
    { &main_consts.Set, OFS_INST_METHODS,  STR_CONST("copy"), subr_new, cf_set_copy },
    { &main_consts.Set, OFS_INST_METHODS,  STR_CONST("copydeep"), subr_new, cf_set_copydeep },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("repr"), subr_new, cf_set_repr },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("tostring"), subr_new, cf_set_repr },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("tolist"), subr_new, cf_set_tolist },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("at"), subr_new, cf_set_at },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("put"), subr_new, cf_set_put },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("freeze"), subr_new, cf_freeze },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("frozenp"), subr_new, cf_frozenp },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("size"), subr_new, cf_set_size },
    { &main_consts.Set, OFS_INST_METHODS, STR_CONST("togenerator"), subr_new, cf_set_toiter },

    { &main_consts.SetIter, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_setiter_new },
    { &main_consts.SetIter, OFS_INST_METHODS, STR_CONST("next"), subr_new, cf_setiter_next },
    { &main_consts.SetIter, OFS_INST_METHODS, STR_CONST("reset"), subr_new, cf_setiter_reset },

    { &main_consts.Dict, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_dict_new },
    { &main_consts.Dict, OFS_INST_METHODS, STR_CONST("at"), subr_new, cf_dict_at },
    { &main_consts.Dict, OFS_INST_METHODS, STR_CONST("ate"), subr_new, cf_dict_ate },
    { &main_consts.Dict, OFS_INST_METHODS, STR_CONST("at-default"), subr_new, cf_dict_at_dflt },    
    { &main_consts.Dict, OFS_INST_METHODS, STR_CONST("atput"), subr_new, cf_dict_atput },
    { &main_consts.Dict, OFS_INST_METHODS, STR_CONST("put"), subr_new, cf_dict_put },
    { &main_consts.Dict, OFS_INST_METHODS, STR_CONST("del"), subr_new, cf_dict_del },
    { &main_consts.Dict, OFS_INST_METHODS, STR_CONST("enter"), nsubr_new, cf_dict_enter },

    { &main_consts.Closure, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_closure_new },
    { &main_consts.Closure, OFS_INST_METHODS,  STR_CONST("copy"), subr_new, cf_closure_copy },
    { &main_consts.Closure, OFS_INST_METHODS,  STR_CONST("copydeep"), subr_new, cf_closure_copydeep },
    { &main_consts.Closure, OFS_INST_METHODS, STR_CONST("repr"), subr_new, cf_closure_repr },
    { &main_consts.Closure, OFS_INST_METHODS, STR_CONST("tostring"), subr_new, cf_closure_repr },
    { &main_consts.Closure, OFS_INST_METHODS, STR_CONST("call"), subr_new, cf_closure_call },
    { &main_consts.Closure, OFS_INST_METHODS, STR_CONST("call1"), subr_new, cf_closure_call1 },
    { &main_consts.Closure, OFS_INST_METHODS, STR_CONST("calln"), subr_new, cf_closure_calln },
    { &main_consts.Closure, OFS_INST_METHODS, STR_CONST("enter"), nsubr_new, cf_closure_enter },
    { &main_consts.Closure, OFS_INST_METHODS, STR_CONST("reduce"), subr_new, cf_reduce_callable },
    { &main_consts.Closure, OFS_INST_METHODS,  STR_CONST("togenerator"), subr_new, cf_closure_togen },

    { &main_consts.ClosureGenerator, OFS_CLASS_METHODS,  STR_CONST("new"), subr_new, cf_closuregen_new },
    { &main_consts.ClosureGenerator, OFS_INST_METHODS,  STR_CONST("next"), subr_new, cf_closuregen_next },
    { &main_consts.ClosureGenerator, OFS_INST_METHODS,  STR_CONST("reset"), subr_new, cf_closuregen_reset },

    { &main_consts.Env, OFS_CLASS_METHODS, STR_CONST("tolist"), subr_new, cf_env_tolist },
    { &main_consts.Env, OFS_CLASS_METHODS, STR_CONST("current"), subr_new, cf_env_current },
    { &main_consts.Env, OFS_CLASS_METHODS, STR_CONST("up"), subr_new, cf_env_up },

    { &main_consts.Module, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_module_new },
    { &main_consts.Module, OFS_CLASS_METHODS, STR_CONST("current"), subr_new, cf_module_current },
    { &main_consts.Module, OFS_INST_METHODS, STR_CONST("repr"), subr_new, cf_module_repr },
    { &main_consts.Module, OFS_INST_METHODS, STR_CONST("tostring"), subr_new, cf_module_repr },
    { &main_consts.Module, OFS_INST_METHODS, STR_CONST("enter"), nsubr_new, cf_module_enter },
    { &main_consts.Module, OFS_INST_METHODS, STR_CONST("at"), subr_new, cf_module_at },
    { &main_consts.Module, OFS_INST_METHODS, STR_CONST("ate"), subr_new, cf_module_ate },

    { &main_consts.Dptr, OFS_CLASS_METHODS, STR_CONST("new"), subr_new, cf_dptr_new },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_dptr_repr },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_dptr_repr },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("tolist"), subr_new, cf_dptr_tolist },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("eval"), nsubr_new, cf_dptr_eval },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("call"), nsubr_new, cf_dptr_call },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("call1"), nsubr_new, cf_dptr_call1 },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("calln"), nsubr_new, cf_dptr_calln },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("next"), subr_new, cf_call0 },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("equal"), subr_new, cf_dptr_equal },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("hash"), subr_new, cf_dptr_hash },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("apply"), subr_new, cf_dptr_apply },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("apply*"), subr_new, cf_dptr_applya },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("atomp"), subr_new, cf_false },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("car"),  subr_new,  cf_dptr_car },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("cadr"), subr_new,  cf_dptr_cadr },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("cdr"),  subr_new,  cf_dptr_cdr },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("append"),  subr_new,  cf_dptr_append },
    { &main_consts.Dptr, OFS_INST_METHODS,  STR_CONST("togenerator"), subr_new, cf_list_toiter },

    { &main_consts.ListIter, OFS_CLASS_METHODS,  STR_CONST("new"), subr_new, cf_listiter_new },    
    { &main_consts.ListIter, OFS_INST_METHODS,  STR_CONST("next"), subr_new, cf_listiter_next },    
    { &main_consts.ListIter, OFS_INST_METHODS,  STR_CONST("reset"), subr_new, cf_listiter_reset },    
    
    { &main_consts.List, OFS_CLASS_METHODS, STR_CONST("new"),    subr_new, cf_list_new },
    { &main_consts.List, OFS_CLASS_METHODS, STR_CONST("dup"),    subr_new, cf_list_dup },
    { &main_consts.List, OFS_CLASS_METHODS, STR_CONST("concat"), subr_new, cf_list_concat },

    { &main_consts.Method, OFS_CLASS_METHODS, STR_CONST("find"), subr_new, cf_method_find },
    { &main_consts.Method, OFS_INST_METHODS,  STR_CONST("call"), subr_new, cf_method_call },
    { &main_consts.Method, OFS_INST_METHODS,  STR_CONST("call1"), subr_new, cf_method_call1 },
    { &main_consts.Method, OFS_INST_METHODS,  STR_CONST("calln"), subr_new, cf_method_calln },
    { &main_consts.Method, OFS_INST_METHODS,  STR_CONST("repr"), subr_new, cf_method_repr },
    { &main_consts.Method, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_method_repr }    
};

void methods_init(unsigned modname_size, const char *modname,
                  unsigned n, const struct method_init *init
                  )
{
    sx_t *work = eval_alloc(2);

    unsigned i;
    for (i = 0; i < n; ++i) {
        struct class *cl = (*init[i].cl)->u.classval->class;
        struct sx_strval *s = cl->name->u.strval;
        unsigned bufsize = s->size - 1 + 2 + init[i].sel_size;
        char *buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
        snprintf(buf, bufsize, "%s::%s", s->data, init[i].sel);
        sym_newm(&work[-1], bufsize, buf);
        (*init[i].constructor)(&work[-2], init[i].func, work[-1]);
        char *p = index(s->data, '.');
        DEBUG_ASSERT(p != 0);
        ++p;
        bufsize = s->size - 1 - (p - s->data) + 2 + init[i].sel_size;
        buf = mem_alloc(bufsize); /* Safe, no exceptions can occur */
        snprintf(buf, bufsize, "%s::%s", p, init[i].sel);
        sym_newc(&work[-1], bufsize, buf);
        env_bind(work[-1], work[-2]);
        sym_newb(&work[-1], init[i].sel_size, init[i].sel);
        dict_atsput((*(sx_t *)((unsigned char *) cl + init[i].dict_ofs))->u.setval,
                    work[-1],
                    sx_hash(work[-1]),
                    work[-2]
                    );
    }

    eval_unwind(work);
}


void methods_destroy(unsigned n, const struct method_init *init)
{
    sx_t *work = eval_alloc(1);

    unsigned i;
    for (i = 0; i < n; ++i) {
        sym_newb(&work[-1], init[i].sel_size, init[i].sel);
        dict_dels((*(sx_t *)((unsigned char *) (*init[i].cl)->u.classval->class + init[i].dict_ofs))->u.setval,
                  work[-1],
                  sx_hash(work[-1])
                  );
    }

    eval_unwind(work);
}


static const struct {
    unsigned   size;
    const char *data;
    sx_t       *_const;
} sym_init_tbl[] = {
    { STR_CONST("t"),            &main_consts.t },
    { STR_CONST("nil"),          &main_consts.nil },
    { STR_CONST("lambda"),       &main_consts.lambda },
    { STR_CONST("nlambda"),      &main_consts.nlambda },
    { STR_CONST("macro"),        &main_consts.macro },
    { STR_CONST("function"),     &main_consts.function },
    { STR_CONST("eval"),         &main_consts.eval },
    { STR_CONST("hash"),         &main_consts.hash },
    { STR_CONST("repr"),         &main_consts.repr },
    { STR_CONST("tostring"),     &main_consts.tostring },
    { STR_CONST("tolist"),       &main_consts.tolist },
    { STR_CONST("equal"),        &main_consts.equal },
    { STR_CONST("cmp"),          &main_consts.cmp },
    { STR_CONST("call"),         &main_consts.call },
    { STR_CONST("call1"),        &main_consts.call1 },
    { STR_CONST("calln"),        &main_consts.calln },
    { STR_CONST("__loaded__"),   &main_consts.__loaded__ },
    { STR_CONST("__init__"),     &main_consts.__init__ },
    { STR_CONST("copy"),         &main_consts.copy },
    { STR_CONST("copydeep"),     &main_consts.copydeep },
    { STR_CONST("default-size"), &main_consts.default_size },
    { STR_CONST("path"),         &main_consts.path },
    { STR_CONST("current"),      &main_consts.current },
    { STR_CONST("togenerator"),  &main_consts.togenerator },
    { STR_CONST("next"),         &main_consts.next },
    { STR_CONST("reset"),        &main_consts.reset },
    { STR_CONST("ate"),          &main_consts.ate },
    { STR_CONST("atput"),        &main_consts.atput },
    { STR_CONST("setq"),         &main_consts.setq }
};


static void syms_init(void)
{
    sx_t *work = eval_alloc(1);
    
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(sym_init_tbl); ++i) {
        sym_newb(&work[-1], sym_init_tbl[i].size, sym_init_tbl[i].data);
        if (sym_init_tbl[i]._const != 0)  *sym_init_tbl[i]._const = work[-1];
    }

    eval_unwind(work);
}


static void env_init(void)
{
    sx_t *work = eval_alloc(7);

    /* eval_stack_top[-1] ::= Set of all defined Symbols */
    sym_set = set_new(&work[-1], 2 * ARRAY_SIZE(init_tbl))->u.setval;
    
    /* eval_stack_top[-2] ::= main module */
    /* eval_stack_top[-3] ::= dict for main module */
    main_consts.module_main = module_new(&work[-2],
                                         dict_new(&work[-3], 2 * ARRAY_SIZE(init_tbl)),
                                         sym_newb(&work[-4], STR_CONST("main")),
                                         0,
                                         0
                                         );
    frame_env_push_module(work[-3], main_consts.module_main);

    syms_init();

    env_bind(main_consts.t, main_consts.t);
    env_bind(main_consts.nil, 0);
    
    sym_newb(&work[-4], STR_CONST("quote"));
    main_consts.quote = nsubr_new(&work[-5], cf_quote, work[-4]);
    env_bind(work[-4], work[-5]);

    sym_newb(&work[-4], STR_CONST("main"));
    env_bind(work[-4], work[-2]);

    funcs_init(STR_CONST("main"), ARRAY_SIZE(init_tbl), init_tbl);

    eval_pop(3);

    _classes_init();
    methods_init(STR_CONST("main"), ARRAY_SIZE(method_init_tbl), method_init_tbl);    
    _classes_init2();

    /* Freeze main module */
    work[-3]->flags.frozen = true;
    
    /* eval_stack[-4] ::= top-level / global env */
    env_global = frame_env_push(dict_new(&work[-4], 64));
}

#ifndef NDEBUG

void debug_sx_validate(sx_t sx)
{
    if (sx == 0)  return;
    if (sx->ref_cnt == 0) {
        fprintf(stderr, "ERROR: ref_cnt == 0 @ %p\n", sx);
    }
    if (sx->type == SX_TYPE_DPTR) {
        debug_sx_validate(car(sx));
        debug_sx_validate(cdr(sx));
    }
}


void debug_sx_print(sx_t sx)
{
    sx_print(stdout, sx);
    putchar('\n');
}


void debug_breakpoint_set(const char *name)
{
    strcpy(debug_breakpoint, name);
}
    

void debug_frame_dump(void)
{
    unsigned i;
    struct frame *p;
    for (i = 0, p = vm->fp; (unsigned char *) p < vm->frame_stack_top; p = p->prev, ++i) {
        printf("%u: %p ", i, p);
        switch (p->type) {
        case FRAME_TYPE_FUNC:
            printf("FRAME_TYPE_FUNC");
            break;
        case FRAME_TYPE_ENV:
            printf("FRAME_TYPE_ENV");
            break;
        case FRAME_TYPE_CLASS:
            printf("FRAME_TYPE_CLASS");
            break;
        case FRAME_TYPE_MEM:
            printf("FRAME_TYPE_MEM");
            break;
        case FRAME_TYPE_EXCEPT:
            printf("FRAME_TYPE_EXCEPT");
            break;
        case FRAME_TYPE_REP:
            printf("FRAME_TYPE_REP");
            break;
        case FRAME_TYPE_PROG:
            printf("FRAME_TYPE_PROG");
            break;
        case FRAME_TYPE_WHILE:
            printf("FRAME_TYPE_WHILE");
            break;
        case FRAME_TYPE_INPUT:
            printf("FRAME_TYPE_INPUT");
            break;
        default:
            printf("INVALID");            
        }
        putchar('\n');
    }
}





#if 0
static void keyboard_interrupt(int dummy __attribute__((unused)))
{
    cons(XFP->arg, str_newb(XFP->arg, STR_CONST("system.keyboard-intr")), 0);
    
    frame_except_raise();
}
#endif

#endif /* NDEBUG */

const char *progname;

static void usage(void)
{
    fprintf(stderr, "usage: %s [-i] [-s <stack-size>] [-S <frame-stack-size>]", progname);
#ifndef NDEBUG
    fprintf(stderr, " [-t]");
#endif
    fprintf(stderr, " [file [args...]]\n");

    myexit(1);
}


int main(int argc, char **argv)
{
    unsigned eval_stack_size = 4096, frame_stack_size = 65536;

    progname = *argv;
    for (--argc, ++argv; argc > 0 && argv[0][0] == '-'; --argc, ++argv) {
        switch (argv[0][1]) {
        case 's':               /* Eval stack size */
            if (argc < 2)  usage();
            ++argv, --argc;
            if (sscanf(*argv, "%u", &eval_stack_size) != 1)  usage();
            break;

        case 'S':               /* Frame stack size */
            if (argc < 2)  usage();
            ++argv, --argc;
            if (sscanf(*argv, "%u", &frame_stack_size) != 1)  usage();
            break;

        case 'P':
            if (argc < 2)  usage();
            ++argv, --argc;
            if (sscanf(*argv, "%u", &mem_sx_page_size) != 1)  usage();
            unsigned sys_page_size = getpagesize();
            if (mem_sx_page_size < sys_page_size) {
              mem_sx_page_size = sys_page_size;
            } else {
              mem_sx_page_size = round_up_to_power_of_2(mem_sx_page_size);
            }
            break;

        case 'e':               /* Force echo of read input */
            echof = true;
            break;

#ifndef NDEBUG
        case 't':
            debug_tracef = true;
            break;
#endif /* NDEBUG */

        case 'h':
            usage();

        default:
            fprintf(stderr, "%s: Invalid option: %s\n", progname, *argv);
            myexit(1);
        }
    }

    stack_init(eval_stack_size, frame_stack_size);

    env_init();

    initf = false;
    
    sx_t *work = eval_alloc(3);

    char *filename;
    static const char mode[] = "r";
    FILE *fp;
    bool interactivef = false;
    if (argc > 0) {
        filename = *argv;
        fp = fopen(filename, mode);
        if (fp == 0) {
            fprintf(stderr, "%s: Cannot open file: %s\n", progname, filename);
            myexit(1);
        }
        --argc;  ++argv;
    } else {
        filename = "stdin";
        fp = stdin;
        if (isatty(fileno(fp)))  interactivef = true;
    }

    str_newb(&work[-1], strlen(filename) + 1, filename);
    str_newb(&work[-2], STR_CONST(mode));
    file_new(&work[-1], work[-1], work[-2], fp);
    
    frame_input_push(work[-1], interactivef);

    sx_assign_nil(&work[-2]);
    sx_t *p = &work[-2];

    for (; argc > 0; --argc, ++argv) {
        char *a = *argv;
        list_append(&p, str_newb(&work[-3], strlen(a) + 1, a));
    }
    env_bind(sym_newb(&work[-3], STR_CONST("argv")), work[-2]);

    eval_pop(2);
    
    if (interactivef) {
        puts("HPLisp 1.0");
    }
    
    rep();

    myexit(0);
}

/* <test> **********************************************************************
> ;; Test callalbe with list implementation
> (def add1 (lambda (x) (add 1 x)))
< (lambda (x) (add 1 x))
> (def sub1 (lambda (x) (sub x 1)))
< (lambda (x) (sub x 1))
> (new Metaclass 'Test Obj)
< <main.Metaclass: Test>
> (atput Test.instance-methods 'call (lambda (self x) (self.func x)))
< (lambda (self x) ((ate self (<main.Nsubr: quote> func)) x))
> (setq tt (new Test))
< <Test: <main.Dict: nil>>
> (tt.func := add1)
< (lambda (x) (add 1 x))
> (tt 1)
< 2
> (tt.func := sub1)
< (lambda (x) (sub x 1))
> (tt 13)
< 12
** </test> **********************************************************************/


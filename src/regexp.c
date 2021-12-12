#include <regex.h>

#include "ll.h"


static struct {
    sx_t Regexp;
} consts;

struct blobdata {
    sx_t    str;
    regex_t re[1];
};

#define SX_BLOBDATA(sx)  ((struct blobdata *)((sx)->u.blobval->data))

static void mark(sx_t sx)
{
    sx_mark(SX_BLOBDATA(sx)->str);
}

static void _free(sx_t sx)
{
    sx_release(SX_BLOBDATA(sx)->str);
}

static void cleanup(sx_t sx)
{
    regfree(SX_BLOBDATA(sx)->re);    
}

static const struct blobhooks hooks[] = {
    { mark, _free, cleanup }
};

static void cf_regexp_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(vm_args());
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);
    
    struct blobdata *r = SX_BLOBDATA(blob_new(vm_dst(), consts.Regexp, hooks, sizeof(*r)));
    if (regcomp(r->re, x->u.strval->data, REG_EXTENDED) != 0) {
        sx_assign_nil(vm_dst());

        return;
    }
    sx_assign_norelease(&r->str, x);
}


static void cf_regexp_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Regexp)  except_bad_arg(x);
    struct sx_strval *s1 = x->u.blobval->inst_of->u.classval->class->name->u.strval;
    struct sx_strval *s2 = SX_BLOBDATA(car(vm_args()))->str->u.strval;
    unsigned bufsize = 1 + s1->size - 1 + 3 + s2->size - 1 + 3;
    char *buf = mem_alloc(bufsize);
    snprintf(buf, bufsize, "<%s: \"%s\">", s1->data, s2->data);
    str_newm(vm_dst(), bufsize, buf);
}


static void match(sx_t *dst, const regex_t *pat, const char *s, unsigned nmatch)
{
    regmatch_t *pmatch = 0;
    regmatch_t matchbuf[nmatch];
    if (nmatch > 0)  pmatch = matchbuf;
    if (regexec(pat, s, nmatch, pmatch, 0) != 0) {
        sx_assign_nil(dst);

        return;
    }
    if (nmatch == 0) {
        sx_assign(dst, main_consts.t);

        return;
    }

    unsigned size, k;
    for (size = 0, pmatch = matchbuf, k = nmatch; k > 0; --k, ++pmatch, ++size) {
        if (pmatch->rm_so == -1)  break;
    }

    sx_t *work = eval_alloc(1);

    struct sx_arrayval *a = array_new(&work[-1], size)->u.arrayval;

    sx_t *p;
    
    for (pmatch = matchbuf, p = a->data; size > 0; --size, ++p, ++pmatch) {
        str_newc(p, pmatch->rm_eo + 1 - pmatch->rm_so, &s[pmatch->rm_so]);
    }

    sx_assign(dst, work[-1]);

    eval_unwind(work);
}


static void cf_regexp_exec(void)
{
    cf_argc_chk_range(2, 3);
    sx_t args = vm_args();
    sx_t x = car(args);
    if (sx_inst_of(x) != consts.Regexp)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (!sx_is_str(y))  except_bad_arg(y);
    intval_t nmatch = 0;
    if (vm_argc() == 3) {
        args = cdr(args);  sx_t z = car(args);
        if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
        nmatch = z->u.intval;
        if (nmatch < 0)    except_bad_arg(z);
    }

    match(vm_dst(), SX_BLOBDATA(x)->re, y->u.strval->data, nmatch);
}


static void cf_regexp_match(void)
{
    cf_argc_chk_range(2, 3);
    sx_t args = vm_args();
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (!sx_is_str(y))  except_bad_arg(y);
    intval_t nmatch = 0;
    if (vm_argc() == 3) {
        args = cdr(args);  sx_t z = car(args);
        if (sx_type(z) != SX_TYPE_INT)  except_bad_arg(z);
        nmatch = z->u.intval;
        if (nmatch < 0)    except_bad_arg(z);
    }

    regex_t re[1];
    if (regcomp(re, x->u.strval->data, REG_EXTENDED) != 0) {
        sx_assign_nil(vm_dst());

        return;
    }

    match(vm_dst(), re, y->u.strval->data, nmatch);

    regfree(re);
}

static const struct class_init classes_init_tbl[] = {
    { &consts.Regexp, STR_CONST("Regexp"), &main_consts.Obj }
};

static const struct method_init methods_init_tbl[] = {
    { &consts.Regexp, OFS_CLASS_METHODS, STR_CONST("new"),      subr_new, cf_regexp_new },
    { &consts.Regexp, OFS_INST_METHODS,  STR_CONST("repr"),     subr_new, cf_regexp_repr },
    { &consts.Regexp, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_regexp_repr },
    { &consts.Regexp, OFS_INST_METHODS,  STR_CONST("exec"),     subr_new, cf_regexp_exec }
};

static const struct func_init funcs_init_tbl[] = {
    { STR_CONST("match"), subr_new, cf_regexp_match }
};

void __regexp_init__(unsigned modname_size, const char *modname)
{
    classes_init(modname_size, modname, ARRAY_SIZE(classes_init_tbl), classes_init_tbl);
    methods_init(modname_size, modname, ARRAY_SIZE(methods_init_tbl), methods_init_tbl);
    funcs_init(modname_size, modname, ARRAY_SIZE(funcs_init_tbl), funcs_init_tbl);
}

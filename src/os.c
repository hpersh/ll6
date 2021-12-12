#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

#include "ll.h"

static void cf_system(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);

    int_new(vm_dst(), system(x->u.strval->data));
}

static struct {
    sx_t Popen, Dir;
} consts;

struct blobdata_popen {
    sx_t cmd, mode;
    FILE *fp;
};

#define SX_BLOBDATA_POPEN(sx)  ((struct blobdata_popen *)((sx)->u.blobval->data))

static void popen_mark(sx_t sx)
{
    struct blobdata_popen *p = SX_BLOBDATA_POPEN(sx);
    sx_mark(p->cmd);
    sx_mark(p->mode);
}

static void popen_free(sx_t sx)
{
    struct blobdata_popen *p = SX_BLOBDATA_POPEN(sx);
    sx_release(p->cmd);
    sx_release(p->mode);
}

static void popen_cleanup(sx_t sx)
{
    pclose(SX_BLOBDATA_POPEN(sx)->fp);
}

static const struct blobhooks hooks_popen[] = {
    { popen_mark, popen_free, popen_cleanup }
};

static void cf_popen_new(void)
{
    cf_argc_chk(3);
    sx_t args = vm_args();
    args = cdr(args);  sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_STR)  except_bad_arg(y);
    FILE *fp = popen(x->u.strval->data, y->u.strval->data);
    if (fp == 0) {
        sx_assign_nil(vm_dst());

        return;
    }

    struct blobdata_popen *p = SX_BLOBDATA_POPEN(blob_new(vm_dst(), consts.Popen, hooks_popen, sizeof(*p)));
    sx_assign_norelease(&p->cmd, x);
    sx_assign_norelease(&p->mode, y);
    p->fp = fp;
}


static void cf_popen_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Popen)  except_bad_arg(x);
    
    struct sx_strval *s1 = x->u.blobval->inst_of->u.classval->class->name->u.strval;
    struct sx_strval *s2 = SX_BLOBDATA_POPEN(x)->cmd->u.strval;
    struct sx_strval *s3 = SX_BLOBDATA_POPEN(x)->mode->u.strval;
    unsigned bufsize = 1 + s1->size - 1 + 3 + s2->size - 1 + 3 + s3->size - 1 + 2 + 1;
    char *buf = mem_alloc(bufsize);
    snprintf(buf, bufsize, "<%s: \"%s\" \"%s\">", s1->data, s2->data, s3->data);
    str_newm(vm_dst(), bufsize, buf);
}


static void cf_popen_readb(void)
{
    cf_argc_chk_range(1, 2);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Popen)  except_bad_arg(x);
    
    unsigned limit = 0;
    bool limit_valid = false;
    if (vm_argc() == 2) {
        sx_t y = cadr(vm_args());
        if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
        intval_t yy = y->u.intval;
        if (yy < 0)  except_bad_arg(y);
        limit = yy;
        limit_valid = true;
    }

    sx_readb(vm_dst(), SX_BLOBDATA_POPEN(x)->fp, limit_valid, limit);
}

struct blobdata_dir {
    sx_t dirname;
    DIR  *dp;
};

#define SX_BLOBDATA_DIR(sx)  ((struct blobdata_dir *)((sx)->u.blobval->data))

static void dir_mark(sx_t sx)
{
    sx_mark(SX_BLOBDATA_DIR(sx)->dirname);
}

static void dir_free(sx_t sx)
{
    sx_release(SX_BLOBDATA_DIR(sx)->dirname);
}

static void dir_cleanup(sx_t sx)
{
    closedir(SX_BLOBDATA_DIR(sx)->dp);
}

static const struct blobhooks hooks_dir[] = {
    { dir_mark, dir_free, dir_cleanup }
};

static void cf_dir_new(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(vm_args());
    if (sx_type(x) != SX_TYPE_STR)  except_bad_arg(x);
    struct sx_strval *s = x->u.strval;
    DIR *dp = opendir(s->data);
    if (dp == 0) {
        sx_assign_nil(vm_dst());

        return;
    }
    
    struct blobdata_dir *d = SX_BLOBDATA_DIR(blob_new(vm_dst(), consts.Dir, hooks_dir, sizeof(*d)));
    sx_assign_norelease(&d->dirname, x);
    d->dp = dp;
}


static void cf_dir_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Dir)  except_bad_arg(x);
    
    struct sx_strval *s1 = x->u.blobval->inst_of->u.classval->class->name->u.strval;
    struct sx_strval *s2 = SX_BLOBDATA_DIR(car(vm_args()))->dirname->u.strval;
    unsigned bufsize = 1 + s1->size - 1 + 2 + s2->size - 1 + 2;
    char *buf = mem_alloc(bufsize);
    snprintf(buf, bufsize, "<%s: %s>", s1->data, s2->data);
    str_newm(vm_dst(), bufsize, buf);
}


static void dict_from_statbuf(sx_t *dst, struct stat *sb)
{
    sx_t *work = eval_alloc(2);

    struct sx_setval *dd = dict_new(dst, 0)->u.setval;

#define STAT_FIELD(f)                                                   \
    sym_newc(&work[-1], STR_CONST(#f));                                 \
    int_new(&work[-2], sb-> f);                                         \
    dict_atsput(dd, work[-1], sx_hash(work[-1]), work[-2]);

    STAT_FIELD(st_dev);
    STAT_FIELD(st_ino);
    STAT_FIELD(st_mode);
    STAT_FIELD(st_nlink);
    STAT_FIELD(st_uid);
    STAT_FIELD(st_gid);
    STAT_FIELD(st_rdev);
    STAT_FIELD(st_size);
    STAT_FIELD(st_blksize);
    STAT_FIELD(st_blocks);
    STAT_FIELD(st_atime);
    STAT_FIELD(st_mtime);
    STAT_FIELD(st_ctime);

    eval_unwind(work);
}


static void cf_file_stat(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_type(x) != SX_TYPE_FILE)   except_bad_arg(x);
    struct stat statbuf[1];
    if (fstat(fileno(x->u.fileval->fp), statbuf) != 0) {
        sx_assign_nil(vm_dst());

        return;
    }
    dict_from_statbuf(vm_dst(), statbuf);
}


static void cf_file_clstat(void)
{
    cf_argc_chk(2);
    sx_t x = cadr(vm_args());
    if (sx_type(x) != SX_TYPE_STR)   except_bad_arg(x);
    struct stat statbuf[1];
    if (stat(x->u.strval->data, statbuf) != 0) {
        sx_assign_nil(vm_dst());

        return;
    }
    dict_from_statbuf(vm_dst(), statbuf);
}


static void cf_dir_stat(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Dir)   except_bad_arg(x);
    struct stat sb[1];
    if (fstat(dirfd(SX_BLOBDATA_DIR(x)->dp), sb) != 0) {
        sx_assign_nil(vm_dst());

        return;
    }

    dict_from_statbuf(vm_dst(), sb);
}


static void cf_dir_readdir(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Dir)  except_bad_arg(x);
    struct dirent *e = readdir(SX_BLOBDATA_DIR(x)->dp);
    if (e == 0) {
        sx_assign_nil(vm_dst());

        return;
    }
    
    struct sx_setval *d = dict_new(vm_dst(), 0)->u.setval;

    sx_t *work = eval_alloc(2);

    sym_newc(&work[-1], STR_CONST("d_ino"));
    int_new(&work[-2], e->d_ino);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_off"));
    int_new(&work[-2], e->d_off);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_reclen"));
    int_new(&work[-2], e->d_reclen);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_type"));
    int_new(&work[-2], e->d_type);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_name"));
    str_newc(&work[-2], strlen(e->d_name) + 1, e->d_name);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);    
}

static const struct class_init classes_init_tbl[] = {
    { &consts.Dir,   STR_CONST("Dir"),   &main_consts.Obj },
    { &consts.Popen, STR_CONST("Popen"), &main_consts.Obj }
};

static const struct method_init methods_init_tbl[] = {
    { &main_consts.File, OFS_CLASS_METHODS, STR_CONST("stat"), subr_new, cf_file_clstat },
    { &main_consts.File, OFS_INST_METHODS, STR_CONST("stat"), subr_new, cf_file_stat },

    { &consts.Dir, OFS_CLASS_METHODS, STR_CONST("new"),      subr_new, cf_dir_new },
    { &consts.Dir, OFS_INST_METHODS,  STR_CONST("repr"),     subr_new, cf_dir_repr },
    { &consts.Dir, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_dir_repr },
    { &consts.Dir, OFS_INST_METHODS,  STR_CONST("stat"),     subr_new, cf_dir_stat },
    { &consts.Dir, OFS_INST_METHODS,  STR_CONST("readdir"),  subr_new, cf_dir_readdir },

    { &consts.Popen, OFS_CLASS_METHODS, STR_CONST("new"),      subr_new, cf_popen_new },
    { &consts.Popen, OFS_INST_METHODS,  STR_CONST("repr"),     subr_new, cf_popen_repr },
    { &consts.Popen, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_popen_repr },
    { &consts.Popen, OFS_INST_METHODS,  STR_CONST("readb"),    subr_new, cf_popen_readb }
};

static const struct func_init funcs_init_tbl[] = {
    { STR_CONST("system"), subr_new, cf_system }
};

void __os_init__(unsigned modname_size, const char *modname)
{
    classes_init(modname_size, modname, ARRAY_SIZE(classes_init_tbl), classes_init_tbl);
    methods_init(modname_size, modname, ARRAY_SIZE(methods_init_tbl), methods_init_tbl);
    funcs_init(modname_size, modname, ARRAY_SIZE(funcs_init_tbl), funcs_init_tbl);
}


void __os_destroy__(void)
{
    methods_destroy(ARRAY_SIZE(methods_init_tbl), methods_init_tbl);
}














#if 0

static void dirtype(sx_t *dst, sx_t sx)
{
    sym_newc(dst, STR_CONST("os:dir"));
}


static inline struct blobval_dir *sx_to_blobval(sx_t sx)
{
    return ((struct blobval_dir *) sx->u.blobval->barray->data);
}


static void dirrepr(sx_t *dst, sx_t sx)
{
    struct blobval_dir *b = sx_to_blobval(sx);
    struct sx_strval *s = b->dirname->u.strval;
    unsigned bufsize = 8 + s->size - 1 + 1 + 1;
    char *buf = mem_alloc(bufsize);
    snprintf(buf, bufsize, "<os:dir %s>", s->data);
    
    str_newm(dst, bufsize, buf);
}


static void dircopy(sx_t *dst __attribute__((unused)),
                    sx_t sx __attribute__((unused))
                    )
{
    except_inv_op();
}


static void dirmark(sx_t sx)
{
    sx_mark(sx_to_blobval(sx)->dirname);
}


static void dircleanup(sx_t sx)
{
    closedir(sx_to_blobval(sx)->dp);
}


static void dirfree(sx_t sx)
{
    sx_release(sx_to_blobval(sx)->dirname);
}

static const struct sx_blobhooks dirhooks[1] = {
    { .type = dirtype,
      .repr = dirrepr,
      .copy = dircopy,
      .copydeep = dircopy,
      .mark = dirmark,
      .free = dirfree,
      .cleanup = dircleanup
    }
};

void cf_opendir(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    sx_t x = car(args);
    if (!sx_is_str(x))   except_bad_arg(x);
    const char *s = x->u.strval->data;

    DIR *dp = opendir(s);
    if (dp == 0) {
        sx_assign_nil(dst);

        return;
    }

    struct blobval_dir *b = (struct blobval_dir *) blob_new(dst, 
    
    struct blobval_dir *b = (struct blobval_dir *) mem_alloc(sizeof(*b));
    sx_assign_norelease(&b->dirname, x);
    b->dp = dp;
    blob_newm(dst, sizeof(*b), (unsigned char *) b, dirhooks);
}


void cf_readdir(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    sx_t x = car(args);
    if (!sx_is_blob(x, dirhooks))  except_bad_arg(x);
    struct dirent *e = readdir(sx_to_blobval(x)->dp);
    if (e == 0) {
        sx_assign_nil(dst);

        return;
    }
    
    struct sx_setval *d = dict_new(dst, 0)->u.setval;

    sx_t *work = eval_alloc(2);

    sym_newc(&work[-1], STR_CONST("d_ino"));
    int_new(&work[-2], e->d_ino);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_off"));
    int_new(&work[-2], e->d_off);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_reclen"));
    int_new(&work[-2], e->d_reclen);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_type"));
    int_new(&work[-2], e->d_type);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
    sym_newc(&work[-1], STR_CONST("d_name"));
    str_newc(&work[-2], strlen(e->d_name) + 1, e->d_name);
    dict_atput(d, work[-1], str_hash(work[-1]->u.strval), work[-2]);
}


void cf_system(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    sx_t x = car(args);
    if (!sx_is_str(x))   except_bad_arg(x);

    int_new(dst, system(x->u.strval->data));
}


void cf_popen(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 2)  except_num_args(2, argc);
    sx_t x = car(args), y = cadr(args);
    int n = list_len(x);
    if (n < 0)  except_bad_arg(x);
    sx_t z;
    for (z = x; z != 0; z = cdr(z)) {
        if (!sx_is_str(car(z)))  except_bad_arg(x);
    }
    if (!sx_is_str(y))   except_bad_arg(y);
    struct sx_strval *s = y->u.strval;

    enum { READ = 1 << 0, WRITE = 1 << 1 };
    
    unsigned m = 0;
    if (strcmp(s->data, "r") == 0) {
        m = READ;
    } else if (strcmp(s->data, "w") == 0) {
        m = WRITE;
    } else if (strcmp(s->data, "rw") == 0) {
        m = READ | WRITE;
    } else  except_bad_arg(y);

    int fd_r[2] = { -1, -1 };
    int fd_w[2] = { -1, -1 };
    if (m & READ) {
        if (pipe(fd_r) < 0) {
            sx_assign_nil(dst);

            return;
        }
    }
    if (m & WRITE) {
        if (pipe(fd_w) < 0) {
            if (m & READ) {
                close(fd_r[0]);
                close(fd_r[1]);
            }
            
            sx_assign_nil(dst);

            return;            
        }
    }

    const char *exec_argv[n + 1];
    unsigned i;
    for (i = 0, z = x; z != 0; z = cdr(z), ++i)  exec_argv[i] = car(z)->u.strval->data;
    exec_argv[i] = 0;

    int pid;
    if ((pid = fork()) == 0) {
        /* Child */

        if (m & READ) {
            close(1);
            if (dup(fd_r[1]) < 0)  goto child_failed;
            close(fd_r[1]);
            close(fd_r[0]);
        }
        if (m & WRITE) {
            close(0);
            if (dup(fd_w[0]) < 0)  goto child_failed;
            close(fd_w[0]);
            close(fd_w[1]);
        }
        
        execv((char *) exec_argv[0], (char **) &exec_argv[1]);

        /* exec failed */

    child_failed:
        exit(1);
    }

    sx_t *work = eval_alloc(5);

    str_newc(&work[-1], STR_CONST("<pipe>"));
    str_newc(&work[-2], STR_CONST("r"));
    str_newc(&work[-3], STR_CONST("w"));

    if (m & WRITE) {
        close(fd_w[0]);
        file_new(&work[-5], work[-1], work[-3], fdopen(fd_w[1], "w"));
        cons(&work[-4], work[-5], work[-4]);
    }
    if (m & READ) {
        close(fd_r[1]);
        file_new(&work[-5], work[-1], work[-2], fdopen(fd_r[0], "r"));
        cons(&work[-4], work[-5], work[-4]);
    }
    int_new(&work[-5], pid);
    cons(dst, work[-5], work[-4]);
}


void cf_wait(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc == 0) {
        int status;
        int pid = wait(&status);

        sx_t *work = eval_alloc(2);

        int_new(&work[-1], pid);
        int_new(&work[-2], status);
        cons(dst, work[-1], work[-2]);

        return;
    }

    if (argc == 1) {
        sx_t x = car(args);
        if (sx_type(x) != SX_TYPE_INT)   except_bad_arg(x);
        
        int status;
        waitpid(x->u.intval, &status, 0);

        int_new(dst, status);

        return;
    }

    except_num_args_range(0, 1, argc);
}


void cf_getenv(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    sx_t x = car(args);
    if (!sx_is_str(x))  except_bad_arg(x);

    char *p = getenv(x->u.strval->data);
    if (p == 0) {
        sx_assign_nil(dst);

        return;
    }

    str_newc(dst, strlen(p) + 1, p);
}

static struct func_init init_tbl[] = {
    { STR_CONST("stat"),    subr_new, cf_stat },
    { STR_CONST("opendir"), subr_new, cf_opendir },
    { STR_CONST("readdir"), subr_new, cf_readdir },
    { STR_CONST("system"),  subr_new, cf_system },
    { STR_CONST("popen"),   subr_new, cf_popen },
    { STR_CONST("wait"),    subr_new, cf_wait },
    { STR_CONST("getenv"),  subr_new, cf_getenv }
};

void __os_init__(sx_t *dst, unsigned modname_size, const char *modname)
{
    funcs_init(dict_new(dst, 32),
               modname_size, modname,
               ARRAY_SIZE(init_tbl), init_tbl
               );
}

#endif

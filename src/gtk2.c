#include <gtk/gtk.h>

#include "ll.h"

/* 
   (setq w (new GtkWindow))
   (call w set-title "Test")
   (call w add-widget (new GtkButton '(label "Test button")))
   (call w show)
   (main)

 */


struct class;

struct gtk_obj {
    const struct class *cls;
    void *ptr;
};

struct class {
    const struct class *parent;
    struct gtkhooks {
        void     (*type)(sx_t *, sx_t);
        void     (*repr)(sx_t *, sx_t);
        void     (*title_set)(struct gtk_obj *, char *);
        void     (*show)(struct gtk_obj *, bool);
        void     (*cleanup)(sx_t);
    } hooks[1];
};


static inline struct gtk_obj *blob_to_gtk_obj(sx_t x)
{
    return ((struct gtk_obj *) x->u.blobval->barray->data);
}


static void _gtk_type(sx_t *dst, sx_t x)
{
    (*blob_to_gtk_obj(x)->cls->hooks->type)(dst, x);
}


static void _gtk_repr(sx_t *dst, sx_t x)
{
    (*blob_to_gtk_obj(x)->cls->hooks->repr)(dst, x);
}


static void _gtk_copy(sx_t *dst, sx_t x)
{
    except_inv_op();
}


static void _gtk_cleanup(sx_t x)
{
    (*blob_to_gtk_obj(x)->cls->hooks->cleanup)(x);
}

static const struct sx_blobhooks hooks[1] = {
    { .type = _gtk_type,
      .repr = _gtk_repr,
      .copy = _gtk_copy,
      .copydeep = _gtk_copy,
      .free = _gtk_cleanup,
      .cleanup = _gtk_cleanup
    }
    /*
    void     (*type)(sx_t *, sx_t);
    void     (*repr)(sx_t *, sx_t);
    void     (*copy)(sx_t *, sx_t);
    void     (*copydeep)(sx_t *, sx_t);
    unsigned (*hash)(sx_t);
    bool     (*equal)(sx_t, sx_t);
    void     (*mark)(sx_t);
    void     (*free)(sx_t);
    void     (*cleanup)(sx_t);
    */
};


void _gtk_widget_show(struct gtk_obj *g, bool allf)
{
    (*(allf ? gtk_widget_show_all : gtk_widget_show))((GtkWidget *) g->ptr);
}


static const struct class cls_widget[1] = {
    { .parent = 0,
      .hooks = { {
          .show = _gtk_widget_show
      } }
    }
};

void _gtk_window_type(sx_t *dst, sx_t x)
{
    str_newc(dst, STR_CONST("gtk-window"));
}


void _gtk_window_repr(sx_t *dst, sx_t x)
{
    str_newc(dst, STR_CONST("<gtk-window>"));
}


void _gtk_window_title_set(struct gtk_obj *g, char *s)
{
    gtk_window_set_title((GtkWindow *) g->ptr, s);
}


void _gtk_window_cleanup(sx_t x)
{
}


static const struct class cls_window[1] = {
    { .parent = cls_widget,
      .hooks = { {
          .type = _gtk_window_type,
          .repr = _gtk_window_repr,
          .title_set = _gtk_window_title_set,
          .show = _gtk_widget_show,
          .cleanup = _gtk_window_cleanup
      } }
    }
};

void _gtk_button_type(sx_t *dst, sx_t x)
{
    str_newc(dst, STR_CONST("gtk-button"));
}


void _gtk_button_repr(sx_t *dst, sx_t x)
{
    str_newc(dst, STR_CONST("<gtk-button>"));
}


void _gtk_button_cleanup(sx_t x)
{
}

#if 0
static const struct class cls_button[1] = {
    { .parent = cls_widget,
      .hooks = { {
          .type = _gtk_button_type,
          .repr = _gtk_button_repr,
          .cleanup = _gtk_button_cleanup
      } }
    }
};
#endif



#define METHOD_CALL(g, sel, ...)                            \
    do {                                                    \
        if (*(g)->cls->hooks-> sel == 0) {                  \
            except_inv_op();                                \
        }                                                   \
        (*(g)->cls->hooks-> sel )((g), ## __VA_ARGS__);     \
    } while (0)
    

 __attribute__((unused)) static bool is_subclass_of(const struct class *cl1, const struct class *cl2)
{
    for (; cl1 != 0; cl1 = cl1->parent) {
        if (cl1 == cl2)  return (true);
    }

    return (false);
}



static void cf_init(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    sx_t x = car(args);
    int n = list_len(x);
    if (n < 0)  except_bad_arg(x);

    char *argv[n], **p;
    sx_t y;
    for (y = x, p = argv; y != 0; y = cdr(y), ++p) {
        sx_t z = car(y);
        if (!sx_is_str(z))  except_bad_arg(x);
        *p = (char *) z->u.strval->data;
    }

    gtk_init(&n, (char ***) &argv);

    sx_assign_nil(dst);
}


static void cf_window_new(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    struct gtk_obj *g = mem_alloc(sizeof(*g));
    g->cls = cls_window;
    g->ptr = gtk_window_new(x->u.intval);

    blob_newm(dst, sizeof(*g), (unsigned char *) g, hooks);
}


static void cf_show(sx_t *dst, unsigned argc, sx_t args)
{
    if (!(argc >= 1 && argc <= 2))  except_num_args_range(1, 2, argc);
    sx_t x = car(args);
    if (!sx_is_blob(x, hooks))  except_bad_arg(x);
    struct gtk_obj *g = blob_to_gtk_obj(x);
    METHOD_CALL(g, show, !(argc == 2 && cadr(args) == 0));
    
    sx_assign_nil(dst);
}


static void cf_title_set(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 2)  except_num_args(2, argc);
    sx_t x = car(args);
    if (!sx_is_blob(x, hooks))  except_bad_arg(x);
    sx_t y = cadr(args);
    if (!sx_is_str(y))  except_bad_arg(y);
    
    struct gtk_obj *g = blob_to_gtk_obj(x);
    METHOD_CALL(g, title_set, (char *) y->u.strval->data);
    
    sx_assign_nil(dst);
}


static void cf_main(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 0)  except_num_args(0, argc);

    gtk_main();

    sx_assign_nil(dst);
}

#define CONST_INIT(nm, val)  { STR_CONST(# nm), SX_TYPE_INT, .u.intval = (val) }

static struct const_init const_tbl[] = {
    CONST_INIT(WINDOW_TOPLEVEL, GTK_WINDOW_TOPLEVEL)
};

static struct func_init init_tbl[] = {
    { STR_CONST("init"),         subr_new, cf_init },
    { STR_CONST("window-new"),   subr_new, cf_window_new },
    { STR_CONST("title-set"),    subr_new, cf_title_set },
    { STR_CONST("show"),         subr_new, cf_show },
    { STR_CONST("main"),         subr_new, cf_main }
};

void __gtk2_init__(sx_t *dst, unsigned modname_size, char *modname)
{
    sx_t d = dict_new(dst, 32);

    consts_init(d, ARRAY_SIZE(const_tbl), const_tbl);

    funcs_init(d,
               modname_size, modname,
               ARRAY_SIZE(init_tbl), init_tbl
               );
}

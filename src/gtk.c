#include <gtk/gtk.h>

#include "ll.h"

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

    int_new(dst, (intptr_t) gtk_window_new(x->u.intval));
}


static void cf_widget_show(sx_t *dst, unsigned argc, sx_t args)
{
    if (!(argc >= 1 && argc <= 2))  except_num_args_range(1, 2, argc);
    sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);

    (*(argc == 2 && cadr(args) == 0 ? gtk_widget_show : gtk_widget_show_all))
        ((GtkWidget *)(intptr_t) x->u.intval)
        ;

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
    { STR_CONST("widget-show"),  subr_new, cf_widget_show },
    { STR_CONST("main"),         subr_new, cf_main }
};

void __gtk_init__(sx_t *dst, unsigned modname_size, char *modname)
{
    sx_t d = dict_new(dst, 32);

    consts_init(d, ARRAY_SIZE(const_tbl), const_tbl);

    funcs_init(d,
               modname_size, modname,
               ARRAY_SIZE(init_tbl), init_tbl
               );
}

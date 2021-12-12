#include <math.h>

#include "ll.h"

void cf_float_sin(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_type(x) != SX_TYPE_FLOAT)  except_bad_arg(x);
    
    float_new(vm_dst(), sinl(x->u.floatval));
}


void cf_float_cos(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_type(x) != SX_TYPE_FLOAT)  except_bad_arg(x);
    
    float_new(vm_dst(), cosl(x->u.floatval));
}

static const struct method_init method_init_tbl[] = {
    { &main_consts.Float, OFS_INST_METHODS, STR_CONST("sin"), subr_new, cf_float_sin },
    { &main_consts.Float, OFS_INST_METHODS, STR_CONST("cos"), subr_new, cf_float_cos }
};

void __math_init__(unsigned modname_size, char *modname)
{
    methods_init(STR_CONST("math"), ARRAY_SIZE(method_init_tbl), method_init_tbl);
}


void __math_destroy__(void)
{
    methods_destroy(ARRAY_SIZE(method_init_tbl), method_init_tbl);
}

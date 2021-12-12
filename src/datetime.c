#include "ll.h"

#include <time.h>

/*
<doc>
## Function: sleep
### Type
subr
### Form
(sleep _secs_)
### Description
Sleep for _secs_ seconds; _secs_ must be a non-negative integer
### Return value
nil
### Exceptions
None
### See also
### Examples
> -> (sleep 1)  
> nil
</doc>
*/

static void cf_sleep(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    intval_t t = x->u.intval;
    if (t < 0)  except_bad_arg(x);

    sleep(t);

    sx_assign_nil(vm_dst());
}


static void cf_time(void)
{
    cf_argc_chk(0);
    int_new(vm_dst(), time(0));
}


static void cf_ctime(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    char buf[26];
    time_t t = x->u.intval;
    ctime_r(&t, buf);
    unsigned n = strlen(buf);
    buf[n - 1] = 0;
    str_newc(vm_dst(), n, buf);
}


static struct func_init init_tbl[] = {
    { STR_CONST("sleep"), subr_new, cf_sleep },
    { STR_CONST("time"),  subr_new, cf_time },
    { STR_CONST("ctime"), subr_new, cf_ctime }
};

void __datetime_init__(unsigned modname_size, char *modname)
{
    funcs_init(modname_size, modname, ARRAY_SIZE(init_tbl), init_tbl);
}

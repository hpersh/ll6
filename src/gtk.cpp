#include <gtkmm/application.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/button.h>

#include "ll.h"

extern "C" {

// GTK instance
// (<type> <ptr> <attrs> <children>)
    
static void gtk_inst_new(sx_t *dst, const char *type, void *ptr)
{
}

    
void cf_gtk_applitcation_window(sx_t *dst, unsigned argc, sx_t args)
{
    gtk_inst_new(dst, "gtk-application-window", new Gtk::ApplicationWindow);
}

    
void cf_gtk_applitcation(sx_t *dst, unsigned argc, sx_t args)
{
    int _argc = 0;
    char **argv = 0;
    auto a = Gtk::Application::create(_argc, argv, "org.gtkmm.example.HelloApp");
    gtk_inst_new(dst, "gtk-application", &*a);
}


void cf_gtk_application_window_add(sx_t *dst, unsigned argc, sx_t args)
{
}

    
void cf_gtk_application_run(sx_t *dst, unsigned argc, sx_t args)
{
}

    
void cf_gtk_call(sx_t *dst, unsigned argc, sx_t args)
{
}

struct {
    const char *type, *sel;
    codefuncval_t func;
} methods_init_tbl[] = {
    { "gtk-application-window", "add", cf_gtk_application_window_add },
    { "gtk-application",        "run", cf_gtk_application_run }
};

static void methods_init(sx_t d)
{
    
}
    

void __gtk_init__(sx_t *dst, unsigned modname_size, char *modname)
{
    sx_t d = dict_new(dst, 32);

    methods_init(d);

#if 0
    funcs_init(d,
               modname_size, modname,
               ARRAY_SIZE(init_tbl), init_tbl
               );
#endif
}
    
}

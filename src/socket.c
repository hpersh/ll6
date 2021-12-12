#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/un.h>

#include "ll.h"

/*
<doc>
## Function: socket
### Type
subr
### Form
(socket _af_ _type_)
### Description
Create a socket, of address family _af_ and type _type_
### Return value
Socket, or nil if failed
### Exceptions
system.bad-argument
### See also
### Examples
> -> (socket AF_INET SOCK_DGRAM)
> <socket>
</doc>
*/

static struct {
    sx_t Socket;
} consts;

struct blobdata {
    int  domain, type;
    sx_t bind_addr;             /* As Barray */
    sx_t connect_addr;          /* As Barray */
    int  fd;
};

#define SX_BLOBDATA(sx)  ((struct blobdata *)((sx)->u.blobval->data))

static void mark(sx_t sx)
{
    sx_mark(SX_BLOBDATA(sx)->bind_addr);
    sx_mark(SX_BLOBDATA(sx)->connect_addr);
}


static void _free(sx_t sx)
{
    sx_release(SX_BLOBDATA(sx)->bind_addr);
    sx_release(SX_BLOBDATA(sx)->connect_addr);
}


static void cleanup(sx_t sx)
{
    close(SX_BLOBDATA(sx)->fd);
}

static const struct blobhooks hooks[1] = {
    { mark, _free, cleanup }
};

static void cf_socket_new(void)
{
    cf_argc_chk(3);
    sx_t args = vm_args();
    args = cdr(args);  sx_t x = car(args);
    if (sx_type(x) != SX_TYPE_INT)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    
    int fd = socket(x->u.intval, y->u.intval, 0);
    if (fd < 0) {
        sx_assign_nil(vm_dst());

        return;
    }

    struct blobdata *s = SX_BLOBDATA(blob_new(vm_dst(), consts.Socket, hooks, sizeof(*s)));
    s->domain = x->u.intval;
    s->type   = y->u.intval;
    s->bind_addr = s->connect_addr = 0;
    s->fd     = fd;
}


static const char *domain_str(int domain)
{
    static const char *tbl[] = {
        "Invalid",
        "AF_UNIX",
        "AF_INET"
    };
    return (domain >= ARRAY_SIZE(tbl) ? "Invalid" : tbl[domain]);
}


static const char *type_str(int type)
{
    static const char *tbl[] = {
        "Invalid",
        "SOCK_STREAM",
        "SOCK_DGRAM"
    };
    return (type >= ARRAY_SIZE(tbl) ? "Invalid" : tbl[type]);
}


static void sockaddr_to_str(sx_t *dst, int domain, unsigned char *addr)
{
    switch (domain) {
    case AF_INET:
        {
            struct sockaddr_in *a = (struct sockaddr_in *) addr;
            char *s = inet_ntoa(a->sin_addr);
            unsigned bufsize = strlen(s) + 1 + 5 + 1;
            char buf[bufsize];
            snprintf(buf, sizeof(buf), "%s:%d", s, ntohs(a->sin_port));
            str_newc1(dst, buf);
        }
        break;

    case AF_UNIX:
        str_newc1(dst, ((struct sockaddr_un *) addr)->sun_path);
        break;
        
    default: ;
    }
}

static void cf_socket_repr(void)
{
    cf_argc_chk(1);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Socket)  except_bad_arg(x);
    struct blobdata *d = SX_BLOBDATA(x);
    struct sx_strval  *s1  = x->u.blobval->inst_of->u.classval->class->name->u.strval;
    const char        *s2  = domain_str(d->domain);
    const char        *s3  = type_str(d->type);

    sx_t *work = eval_alloc(2);

    struct sx_strval *s4 = 0;
    if (d->bind_addr != 0) {
        sockaddr_to_str(&work[-1], d->domain, d->bind_addr->u.barrayval->data);
        s4 = work[-1]->u.strval;
    }

    struct sx_strval *s5 = 0;
    if (d->connect_addr != 0) {
        sockaddr_to_str(&work[-2], d->domain, d->connect_addr->u.barrayval->data);
        s5 = work[-2]->u.strval;
    }

    unsigned bufsize = 1 + s1->size - 1 + 2 + strlen(s2) + 1 + strlen(s3)
        + (s4 == 0 ? 0 : 1 + s4->size - 1)
        + (s5 == 0 ? 0 : 1 + s5->size - 1)
        + 1 + 1;
    char *buf = mem_alloc(bufsize);
    snprintf(buf, bufsize, "<%s: %s %s%s%s%s%s>",
             s1->data,
             s2,
             s3,
             s4 == 0 ? "" : " ",
             s4 == 0 ? "" : s4->data,
             s5 == 0 ? "" : " ",
             s5 == 0 ? "" : s5->data
             );
    str_newm(vm_dst(), bufsize, buf);
}


static bool sx_sockaddr(sx_t *dst, sx_t sx)
{
    if (sx_is_str(sx)) {
        struct sx_strval *s = sx->u.strval;
        struct sockaddr_un sockaddr[1];
        if (s->size > sizeof(sockaddr->sun_path))  return (false);
        memset(sockaddr, 0, sizeof(*sockaddr));
        strcpy(sockaddr->sun_path, s->data);
        sockaddr->sun_family = AF_UNIX;
        barray_newc(dst, sizeof(*sockaddr), (unsigned char *) sockaddr);

        return (true);
    }
    if (list_len(sx) == -2) {
        sx_t sxcar = car(sx), sxcdr = cdr(sx);
        if (!sx_is_str(sxcar))  return (false);
        struct sockaddr_in sockaddr[1];
        if (inet_aton(sxcar->u.strval->data, &sockaddr->sin_addr) != 1)  return (false);
        if (sx_type(sxcdr) != SX_TYPE_INT)  return (false);
        intval_t port = sxcdr->u.intval;
        if (!(port >= 0 && port <= 65535))  return (false);
        sockaddr->sin_port = htons(port);
        sockaddr->sin_family = AF_INET;
        
        barray_newc(dst, sizeof(*sockaddr), (unsigned char *) sockaddr);

        return (true);
    }

    return (false);
}


static void cf_socket_bind(void)
{
    cf_argc_chk(2);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Socket)  except_bad_arg(x);
    struct blobdata *d = SX_BLOBDATA(x);
    sx_t y = cadr(vm_args());

    sx_t *work = eval_alloc(1);

    if (!sx_sockaddr(&work[-1], y))  except_bad_arg(y);
    struct sx_barrayval *b = work[-1]->u.barrayval;
    int result = bind(d->fd, (struct sockaddr *) b->data, b->size);
    if (result == 0)  sx_assign(&d->bind_addr, work[-1]);

    int_new(vm_dst(), result);
}


static void cf_socket_connect(void)
{
    cf_argc_chk(2);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Socket)  except_bad_arg(x);
    struct blobdata *d = SX_BLOBDATA(x);
    sx_t y = cadr(vm_args());

    sx_t *work = eval_alloc(1);

    if (!sx_sockaddr(&work[-1], y))  except_bad_arg(y);
    struct sx_barrayval *b = work[-1]->u.barrayval;
    int result = connect(d->fd, (struct sockaddr *) b->data, b->size);
    if (result == 0)  sx_assign(&d->connect_addr, work[-1]);

    int_new(vm_dst(), result);
}


static bool sx_size_data(sx_t sx, unsigned *size, unsigned char **data)
{
    switch (sx_type(sx)) {
    case SX_TYPE_STR:
    case SX_TYPE_SYM:
        {
            struct sx_strval *s = sx->u.strval;
            *size = s->size - 1;
            *data = (unsigned char *) s->data;
        }
        return (true);

    case SX_TYPE_BARRAY:
        {
            struct sx_barrayval *b = sx->u.barrayval;
            *size = b->size;
            *data = b->data;
        }
        return (true);
        
    default: ;
    }

    return (false);
}


static void cf_socket_send(void)
{
    cf_argc_chk(2);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Socket)  except_bad_arg(x);
    sx_t y = cadr(vm_args());
    unsigned char *data = 0;
    unsigned size = 0;
    if (!sx_size_data(y, &size, &data))  except_bad_arg(y);

    int_new(vm_dst(), send(SX_BLOBDATA(x)->fd, data, size, 0));
}


static void cf_socket_recv(void)
{
    cf_argc_chk(2);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Socket)  except_bad_arg(x);
    sx_t y = cadr(vm_args());
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(x);
    intval_t readsize = y->u.intval;
    if (y < 0)  except_bad_arg(y);

    unsigned char *buf = mem_alloc(readsize);
    int n = recv(SX_BLOBDATA(x)->fd, buf, readsize, 0);
    if (n < 0) {
        sx_assign_nil(vm_dst());

        return;
    }

    barray_newm(vm_dst(), n, mem_realloc(buf, readsize, n));
}


static void cf_socket_sendto(void)
{
    cf_argc_chk(3);
    sx_t args = vm_args();
    sx_t x = car(args);
    if (sx_inst_of(x) != consts.Socket)  except_bad_arg(x);
    args = cdr(args);  sx_t y = car(args);
    unsigned size = 0;
    unsigned char *data = 0;
    if (!sx_size_data(y, &size, &data))  except_bad_arg(y);
    args = cdr(args);  sx_t z = car(args);

    sx_t *work = eval_alloc(1);

    if (!sx_sockaddr(&work[-1], z))  except_bad_arg(z);
    struct sx_barrayval *b = work[-1]->u.barrayval;

    int_new(vm_dst(), sendto(SX_BLOBDATA(x)->fd,
                             data,
                             size,
                             0,
                             (struct sockaddr *) b->data, b->size
                             )
            );
}


static void cf_socket_recvfrom(void)
{
    cf_argc_chk(2);
    sx_t x = car(vm_args());
    if (sx_inst_of(x) != consts.Socket)  except_bad_arg(x);
    sx_t y = cadr(vm_args());
    if (sx_type(y) != SX_TYPE_INT)  except_bad_arg(y);
    intval_t len = y->u.intval;
    if (len < 0)  except_bad_arg(y);
    unsigned char *buf = malloc(len);
    struct sockaddr_un sockaddr[1];
    socklen_t sockaddr_len = sizeof(*sockaddr);
    ssize_t n = recvfrom(SX_BLOBDATA(x)->fd, buf, len, 0, (struct sockaddr *) sockaddr, &sockaddr_len);
    if (n < 0) {
        sx_assign_nil(vm_dst());

        return;
    }

    sx_t *work = eval_alloc(2);

    switch (sockaddr->sun_family) {
    case AF_UNIX:
        str_newc(&work[-1], strlen(sockaddr->sun_path) + 1, sockaddr->sun_path);
        break;

    case AF_INET:
        {
            struct sockaddr_in *sockaddr_in = (struct sockaddr_in *) sockaddr;
            char *p = inet_ntoa(sockaddr_in->sin_addr);
            str_newc(&work[-1], strlen(p) + 1, p);
            int_new(&work[-2], ntohs(sockaddr_in->sin_port));
            cons(&work[-1], work[-1], work[-2]);
        }
        break;
        
    default:
        sx_assign_nil(vm_dst());

        return;
    }

    barray_newm(&work[-2], n, realloc(buf, n));
    cons(vm_dst(), work[-1], work[-2]);
}

#define CONST_INIT(x)  { STR_CONST(#x), SX_TYPE_INT, .u.intval = x }

static struct const_init consts_tbl[] = {
    CONST_INIT(AF_INET),
    CONST_INIT(AF_UNIX),
    CONST_INIT(SOCK_DGRAM),
    CONST_INIT(SOCK_STREAM)
};

static const struct class_init classes_init_tbl[] = {
    { &consts.Socket, STR_CONST("Socket"), &main_consts.Obj }
};

static const struct method_init methods_init_tbl[] = {
    { &consts.Socket, OFS_CLASS_METHODS, STR_CONST("new"),      subr_new, cf_socket_new },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("repr"),     subr_new, cf_socket_repr },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("tostring"), subr_new, cf_socket_repr },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("bind"),     subr_new, cf_socket_bind },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("connect"),  subr_new, cf_socket_connect },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("send"),     subr_new, cf_socket_send },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("recv"),     subr_new, cf_socket_recv },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("sendto"),   subr_new, cf_socket_sendto },
    { &consts.Socket, OFS_INST_METHODS,  STR_CONST("recvfrom"), subr_new, cf_socket_recvfrom }
};

void __socket_init__(unsigned modname_size, char *modname)
{
    consts_init(ARRAY_SIZE(consts_tbl), consts_tbl);

    classes_init(modname_size, modname, ARRAY_SIZE(classes_init_tbl), classes_init_tbl);
    methods_init(modname_size, modname, ARRAY_SIZE(methods_init_tbl), methods_init_tbl);
}

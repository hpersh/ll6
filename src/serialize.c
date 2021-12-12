#include "ll.h"

enum {
    SERIALIZE_TYPE_NIL = 0,
    SERIALIZE_TYPE_INT,
    SERIALIZE_TYPE_FLOAT,
    SERIALIZE_TYPE_STR,
    SERIALIZE_TYPE_SYM,
    SERIALIZE_TYPE_BARRAY,
    SERIALIZE_TYPE_LIST_BEGIN,
    SERIALIZE_TYPE_LIST_END,
    SERIALIZE_TYPE_LIST_END_CDR,
    SERIALIZE_TYPE_ARRAY,
    SERIALIZE_TYPE_SET_BEGIN,
    SERIALIZE_TYPE_SET_END,
    SERIALIZE_TYPE_DICT_BEGIN,
    SERIALIZE_TYPE_DICT_END,
    SERIALIZE_TYPE_FROZEN = 0x80
};

bool sx_serialize_len_read(struct stream *s, unsigned *len)
{
    unsigned char b;
    if (stream_read(s, &b, 1) != 1)  return (false);
    switch (b >> 6) {
    case 0:
        *len = b;

        return (true);

    case 1:
        {
            unsigned char b2;
            if (stream_read(s, &b2, 1) != 1)  return (false);

            *len = ((b & 0x3f) << 8) | b2;
        }
        return (true);

    case 2:
        {
            unsigned char b2[2];
            if (stream_read(s, b2, 2) != 2)  return (false);

            *len = ((b & 0x3f) << 16) | (b2[0] << 8) | b2[1];
        }
        return (true);

    default: ;
    }

    unsigned char b2[4];
    if (stream_read(s, b2, 4) != 4)  return (false);
    
    *len = (b2[0] << 24) | (b2[1] << 16) | (b2[2] << 8) | b2[3];

    return (true);
}


void sx_serialize_len_write(struct ebuf *eb, unsigned len)
{
    if (len < (1 << 6)) {
        ebuf_appendc(eb, len);

        return;
    }
    if (len < (1 << (6 + 8))) {
        ebuf_appendc(eb, (1 << 6) | (len >> 8));
        ebuf_appendc(eb, len);

        return;
    }
    if (len < (1 << (6 + 2 * 8))) {
        ebuf_appendc(eb, (2 << 6) | (len >> 16));
        ebuf_appendc(eb, len >> 8);
        ebuf_appendc(eb, len);

        return;
    }

    ebuf_appendc(eb, 3 << 6);
    unsigned sh, n;
    for (sh = 24, n = 4; n > 0; --n, sh -= 8)  ebuf_appendc(eb, len >> sh);
}


bool sx_serialize_write(struct ebuf *eb, sx_t sx)
{
    unsigned t = sx_type(sx);
    switch (t) {
    case SX_TYPE_NIL:
        ebuf_appendc(eb, SERIALIZE_TYPE_NIL);
        break;

    case SX_TYPE_INT:
        ebuf_appendc(eb, SERIALIZE_TYPE_INT);
        ebuf_append(eb, sizeof(sx->u.intval), (unsigned char *) &sx->u.intval);
        break;

    case SX_TYPE_FLOAT:
        ebuf_appendc(eb, SERIALIZE_TYPE_FLOAT);
        ebuf_append(eb, sizeof(sx->u.floatval), (unsigned char *) &sx->u.floatval);
        break;

    case SX_TYPE_STR:
    case SX_TYPE_SYM:
    case SX_TYPE_BARRAY:
        {
            unsigned type = 0;
            switch (t) {
            case SX_TYPE_STR:     type = SERIALIZE_TYPE_STR;     break;
            case SX_TYPE_SYM:     type = SERIALIZE_TYPE_SYM;     break;
            case SX_TYPE_BARRAY:  type = SERIALIZE_TYPE_BARRAY;  break;
            default:
                assert(0);
            }
            ebuf_appendc(eb, type);
            unsigned size;
            unsigned char *data;
            if (t == SX_TYPE_BARRAY) {
                size = sx->u.barrayval->size;
                data = sx->u.barrayval->data;
            } else {
                size = sx->u.strval->size - 1;
                data = (unsigned char *) sx->u.strval->data;
            }
            sx_serialize_len_write(eb, size);
            ebuf_append(eb, size, data);
        }
        break;
        
    case SX_TYPE_ARRAY:
        {
            ebuf_appendc(eb, SERIALIZE_TYPE_ARRAY);
            struct sx_arrayval *a = sx->u.arrayval;
            sx_serialize_len_write(eb, a->size);
            sx_t *p;
            unsigned n;
            for (p = a->data, n = a->size; n > 0; --n, ++p) {
                sx_serialize_write(eb, *p);
            }
        }
        break;

    case SX_TYPE_SET:
        {
            ebuf_appendc(eb, SERIALIZE_TYPE_SET_BEGIN);
            struct sx_arrayval *a = sx->u.setval->array;
            sx_serialize_len_write(eb, a->size);
            sx_t *p;
            unsigned n;
            for (p = a->data, n = a->size; n > 0; --n, ++p) {
                sx_t li;
                for (li = *p; li != 0; li = cdr(li)) {
                    sx_serialize_write(eb, car(li));
                }
            }
            ebuf_appendc(eb, SERIALIZE_TYPE_SET_END);
        }
        break;

    case SX_TYPE_DICT:
        {
            ebuf_appendc(eb, SERIALIZE_TYPE_DICT_BEGIN);
            struct sx_arrayval *a = sx->u.setval->array;
            sx_serialize_len_write(eb, a->size);
            sx_t *p;
            unsigned n;
            for (p = a->data, n = a->size; n > 0; --n, ++p) {
                sx_t li;
                for (li = *p; li != 0; li = cdr(li)) {
                    sx_t i = car(li);
                    sx_serialize_write(eb, car(i));
                    sx_serialize_write(eb, cdr(i));
                }
            }
            ebuf_appendc(eb, SERIALIZE_TYPE_DICT_END);
        }
        break;

    case SX_TYPE_DPTR:
        ebuf_appendc(eb, SERIALIZE_TYPE_LIST_BEGIN);
        for (; sx != 0; sx = cdr(sx)) {
            if (sx_type(sx) != SX_TYPE_DPTR) {
                if (!sx_serialize_write(eb, sx))  return (false);
                ebuf_appendc(eb, SERIALIZE_TYPE_LIST_END_CDR);
                
                return (true);
            }

            if (!sx_serialize_write(eb, car(sx)))  return (false);
        }
        ebuf_appendc(eb, SERIALIZE_TYPE_LIST_END);

        break;
        
    default:
        return (false);
    }

    return (true);
}


bool sx_serialize_read(sx_t *dst, struct stream *s)
{
    bool result = true;

    unsigned char b;
    if (stream_read(s, &b, 1) != 1)  return (false);
    switch (b) {
    case SERIALIZE_TYPE_NIL:
        sx_assign_nil(dst);

        break;

    case SERIALIZE_TYPE_INT:
        {
            intval_t val;
            if (stream_read(s, (unsigned char *) &val, sizeof(val)) != sizeof(val))  return (false);
            int_new(dst, val);
        }
        break;

    case SERIALIZE_TYPE_FLOAT:
        {
            floatval_t val;
            if (stream_read(s, (unsigned char *) &val, sizeof(val)) != sizeof(val))  return (false);
            float_new(dst, val);
        }
        break;

    case SERIALIZE_TYPE_STR:
    case SERIALIZE_TYPE_SYM:
    case SERIALIZE_TYPE_BARRAY:
        {
            unsigned n;
            if (!sx_serialize_len_read(s, &n))  return (false);
            unsigned char *buf = mem_alloc(b == SERIALIZE_TYPE_BARRAY ? n : n + 1);
            if (stream_read(s, buf, n) != n) {
                mem_free(n, buf);

                return (false);
            }
            switch (b) {
            case SERIALIZE_TYPE_STR:
                buf[n] = 0;
                str_newm(dst, n + 1, (char *) buf);
                break;
                
            case SERIALIZE_TYPE_SYM:
                buf[n] = 0;
                sym_newm(dst, n + 1, (char *) buf);
                break;
                
            case SERIALIZE_TYPE_BARRAY:
                barray_newm(dst, n, buf);
                break;

            default:
                assert(0);
            }
        }
        break;

    case SERIALIZE_TYPE_ARRAY:
        {
            unsigned n;
            if (!sx_serialize_len_read(s, &n))  return (false);
            sx_t *p = array_new(dst, n, 0)->u.arrayval->data;
            for (; n > 0; --n, ++p) {
                if (!sx_serialize_read(p, s))  return (false);
            }
        }
        break;

    case SERIALIZE_TYPE_SET_BEGIN:
        {
            unsigned n;
            if (!sx_serialize_len_read(s, &n))  return (false);
            struct sx_setval *ss = set_new(dst, n)->u.setval;

            sx_t *work = eval_alloc(1);
            
            for (;;) {
                unsigned char bb;
                if (stream_read(s, &bb, 1) != 1) {
                    result = false;

                    break;
                }
                if (bb == SERIALIZE_TYPE_SET_END)  break;
                stream_ungetc(s, bb);
                if (!sx_serialize_read(&work[-1], s)) {
                    result = false;

                    break;
                }

                set_put(ss, work[-1], sx_hash(work[-1]));
            }

            eval_unwind(work);
        }
        break;

    case SERIALIZE_TYPE_DICT_BEGIN:
        {
            unsigned n;
            if (!sx_serialize_len_read(s, &n))  return (false);
            struct sx_setval *ss = dict_new(dst, n)->u.setval;

            sx_t *work = eval_alloc(2);
            
            for (;;) {
                unsigned char bb;
                if (stream_read(s, &bb, 1) != 1) {
                    result = false;

                    break;
                }
                if (bb == SERIALIZE_TYPE_DICT_END)  break;
                stream_ungetc(s, bb);
                if (!(sx_serialize_read(&work[-1], s) && sx_serialize_read(&work[-2], s))) {
                    result = false;

                    break;
                }
                
                dict_atput(ss, work[-1], sx_hash(work[-1]), work[-2]);
            }
            
            eval_unwind(work);
        }
        break;

    case SERIALIZE_TYPE_LIST_BEGIN:
        {
            sx_t *work = eval_alloc(2), *p = &work[-1], *q = 0;
            
            for (;;) {
                unsigned char bb;
                if (stream_read(s, &bb, 1) != 1) {
                    result = false;

                    break;
                }
                
                if (bb == SERIALIZE_TYPE_LIST_END_CDR) {
                    sx_assign(q, car(*q));
                    break;
                }
                if (bb == SERIALIZE_TYPE_LIST_END)  break;
                   
                stream_ungetc(s, bb);
                if (!sx_serialize_read(&work[-2], s)) {
                    result = false;

                    break;
                }

                q = p;
                p = &cons(p, work[-2], 0)->u.dptrval->cdr;
            }

            eval_unwind(work);
        }
        break;        

    default:
        return (false);
    }

    return (result);
}


void cf_serialize_read(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    sx_t x = car(args);
    union {
        struct stream_file stream_file[1];
        struct stream_str  stream_str[1];
    } u;
    struct stream *s;
    switch (sx_type(x)) {
    case SX_TYPE_BARRAY:
        {
            struct sx_barrayval *b = x->u.barrayval;
            stream_str_init(u.stream_str, b->size, b->data);
            s = u.stream_str->base;
        }
        break;
        
    case SX_TYPE_FILE:
        stream_file_init(u.stream_file, sx_file_fp(x));
        s = u.stream_file->base;
        break;
        
    default:
        except_bad_arg(x);
    }

    if (!sx_serialize_read(dst, s))  except_bad_arg(x);
}


void cf_serialize_write(sx_t *dst, unsigned argc, sx_t args)
{
    if (argc != 1)  except_num_args(1, argc);
    
    struct ebuf eb[1];
    ebuf_init(eb, 1024);
    sx_t x = car(args);
    if (!sx_serialize_write(eb, x))  except_bad_arg(x);
    barray_newm(dst, ebuf_size(eb), ebuf_finis(eb));
}

static struct func_init init_tbl[] = {
    { STR_CONST("write"), subr_new, cf_serialize_write },
    { STR_CONST("read"),  subr_new, cf_serialize_read }
};

void __serialize_init__(sx_t *dst, unsigned modname_size, char *modname)
{
    funcs_init(dict_new(dst, 32),
               modname_size, modname,
               ARRAY_SIZE(init_tbl), init_tbl
               );
}

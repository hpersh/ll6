void thread_type(struct thread *th, sx_t *dst, sx_t sx)
{
    str_newc(th, dst, STR_CONST("thread"));
}                


void thread_repr(struct thread *th, sx_t *dst, sx_t sx)
{
    char bufsize = 8 + 32 + 1 + 1;
    char buf[bufsize];
    snprintf(buf, bufsize, "<thread %lu>", sx_threadval(sx)->id);
    str_newc(th, dst, strlen(buf) + 1, buf);
}

__attribute__((noreturn)) void except_inv_op(struct thread *th);

void thread_copy(struct thread *th, sx_t *dst, sx_t sx)
{
    except_inv_op(th);
}


unsigned thread_hash(struct thread *th, sx_t sx)
{
    return (sx_threadval(sx)->id);
}


bool thread_equal(struct thread *th, sx_t sx1, sx_t sx2)
{
    return (sx1 == sx2);
}


__attribute__((noreturn)) int thread_cmp(struct thread *th, sx_t sx1, sx_t sx2)
{
    except_inv_op(th);
}


void _thread_mark(struct thread *th)
{
    sx_t *p;
    for (p = th->vm_state->sp; p < th->eval_stack_top; ++p)  sx_mark(*p);
}


void thread_mark(sx_t sx)
{
    _thread_mark(sx_threadval(sx));
}


void thread_cleanup(sx_t sx)
{
    struct thread *th = sx_threadval(sx);
    pthread_cancel(th->id);
    pthread_join(th->id, 0);
    
    mem_free(th->stack_size, th->eval_stack);
    mem_free(th->frame_stack_size, th->frame_stack);
}

static const struct blobhooks thread_hooks[1] = { {
        .type     = thread_type,
        .repr     = thread_repr,
        .copy     = thread_copy,
        .copydeep = thread_copy,
        .hash     = thread_hash,
        .equal    = thread_equal,
        .cmp      = thread_cmp,
        .mark     = thread_mark,
        .cleanup  = thread_cleanup
    } };

sx_t thread_newm(struct thread *th, sx_t *dst, struct thread *newth)
{
    return (blob_newm(th, dst, sizeof(*newth), (unsigned char *) newth, thread_hooks));
}



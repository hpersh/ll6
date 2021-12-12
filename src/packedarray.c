
enum {
    SX_TYPE_BOOL = SX_TYPE_LAST
};

struct hdr {
    unsigned char  type, type_sizeof;
    unsigned short rank;
};


static unsigned type_size(sx_t type)
{
    sx_t *work = eval_alloc(1);

    sym_newc(&work[-1], STR_CONST("int"));
    if (sx_equal(type, work[-1]))  return (sizeof(intval_t));
    sym_newc(&work[-1], STR_CONST("float"));
    if (sx_equal(type, work[-1]))  return (sizeof(floatval_t));
    sym_newc(&work[-1], STR_CONST("complex"));
    if (sx_equal(type, work[-1]))  return (sizeof(struct sx_complexval));

    return (0);
}

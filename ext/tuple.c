#include "ruby.h"
#include <netinet/in.h>

VALUE mTuple;
VALUE rb_cDate;

#define TRUE_SORT  255 // TrueClass
#define TUPLE_SORT 192 // Array
#define TUPLE_END  191 // For nested tuples 
#define TIME_SORT  128 // Time
#define SYM_SORT    64 // Symbol
#define STR_SORT    32 // String
#define INTP_SORT   16 // Integer (Positive)
#define INTN_SORT    8 // Integer (Negative)
#define FALSE_SORT   1 // FalseClass
#define NIL_SORT     0 // NilClass


static void null_pad(VALUE data, int len) {
  static u_int8_t null = 0;

  // Pad with null bytes so subsequent fields will be aligned.
  while (len % 4 != 0) {
    rb_str_cat(data, (char*)&null, 1);
    len++;
  }
}


u_int32_t split64(int64_t num, int word) {
  u_int32_t *split = (u_int32_t*)(void*)&num;

  static int i = 1;
  if (*(char *)&i == 1) word = word ? 1: 0;
  else                  word = word ? 0: 1;

  return split[word];
}


//typedef unsigned long BDIGIT;

/*
 * call-seq:
 * Tuple.dump(tuple) -> string
 *
 * Dumps an array of simple Ruby types into a string of binary data.
 *
 */
static VALUE tuple_dump(VALUE self, VALUE tuple) {
    VALUE data = rb_str_new2("");
    VALUE item;
    int i, j, len, sign, byte_len, padded;
    u_int8_t header[4];
    u_int32_t digit;
    int64_t fixnum;
    unsigned char *buf;
    uint32_t v;

    if (TYPE(tuple) != T_ARRAY) tuple = rb_ary_new4(1, &tuple); // 不是数组转成数组
    for (i = 0; i < RARRAY_LEN(tuple); i++) {
        item = RARRAY_PTR(tuple)[i];

        header[0] = header[1] = header[2] = header[3] = 0;
        if (FIXNUM_P(item)) {
            fixnum = FIX2LONG(item);
            sign = (fixnum >= 0);
            if (!sign) fixnum = -fixnum;
            len = fixnum > UINT_MAX ? 2 : 1;
            header[2] = sign ? INTP_SORT : INTN_SORT;
            header[3] = sign ? len : UCHAR_MAX - len;
            rb_str_cat(data, (char*)&header, sizeof(header));      

            if (len == 2) {
            digit = split64(fixnum, 1);
            digit = htonl(sign ? digit : UINT_MAX - digit);
            rb_str_cat(data, (char*)&digit, sizeof(digit));
            }
            digit = split64(fixnum, 0);
            digit = htonl(sign ? digit : UINT_MAX - digit);
            rb_str_cat(data, (char*)&digit, sizeof(digit));
        } else if (TYPE(item) == T_BIGNUM) { // 数字
            sign = RBIGNUM_SIGN(item);

            //len  = RBIGNUM_LEN(item);

            /* 1. 需要多少字节表示绝对值 */
            byte_len = rb_absint_size(item, NULL);
            /* 2. 向上补齐到 4 字节 */
            padded = (byte_len + 3) & ~3;
            /* 3. 等价于 RBIGNUM_LEN */
            len = padded / 4;

            header[2] = sign ? INTP_SORT : INTN_SORT;
            header[3] = sign ? len : UCHAR_MAX - len;
            rb_str_cat(data, (char*)&header, sizeof(header));

            // digits = RBIGNUM_DIGITS(item);
            // for (j = len-1; j >= 0; j--) {
            //     digit = htonl(sign ? digits[j] : (UINT_MAX - digits[j]));
            //     rb_str_cat(data, (char*)&digit, sizeof(digit));
            // }


            /* 3. byte buffer */
            buf = ALLOCA_N(unsigned char, padded);
            memset(buf, 0, padded);

            /* 4. 用官方 API 导出整数（字节级） */
            rb_integer_pack(
                item,
                buf + (padded - byte_len), /* 右对齐 */
                byte_len,
                1,     /* 1 byte per word */
                0,
                INTEGER_PACK_BIG_ENDIAN 
            );

            /* 5. 每 4 字节拼成 uint32_t */
            for (j = 0; j < len; j++) {
                v =
                    ((uint32_t)buf[j*4]   << 24) |
                    ((uint32_t)buf[j*4+1] << 16) |
                    ((uint32_t)buf[j*4+2] <<  8) |
                    ((uint32_t)buf[j*4+3]);

                if (!sign) {
                    v = UINT_MAX - v;
                }

                v = htonl(v);
                rb_str_cat(data, (char*)&v, sizeof(v));
            }
        } else if (SYMBOL_P(item) || TYPE(item) == T_STRING) {
            if (SYMBOL_P(item)) {
                header[2] = SYM_SORT;
                item = rb_funcall(item, rb_intern("to_s"), 0);
            } else {
                header[2] = STR_SORT;
            }
            rb_str_cat(data, (char*)&header, sizeof(header));
            len = RSTRING_LEN(item);
            rb_str_cat(data, RSTRING_PTR(item), len);

            null_pad(data, len);
        } else if (rb_obj_class(item) == rb_cTime || rb_obj_class(item) == rb_cDate) {
            header[2] = TIME_SORT;
            rb_str_cat(data, (char*)&header, sizeof(header));

            if (rb_obj_class(item) == rb_cTime) {
                item = rb_funcall(item, rb_intern("getgm"), 0);
                item = rb_funcall(item, rb_intern("strftime"), 1, rb_str_new2("%Y/%m/%d %H:%M:%S +0000"));
            } else {
                item = rb_funcall(item, rb_intern("strftime"), 1, rb_str_new2("%Y/%m/%d"));
            }
            len = RSTRING_LEN(item);
            rb_str_cat(data, RSTRING_PTR(item), len);

            null_pad(data, len);
        } else if (TYPE(item) == T_ARRAY) {
            header[2] = TUPLE_SORT;
            rb_str_cat(data, (char*)&header, sizeof(header));

            rb_str_concat(data, tuple_dump(mTuple, item));

            header[2] = TUPLE_END;
            rb_str_cat(data, (char*)&header, sizeof(header));      
        } else {
            if (item == Qnil)   
                header[2] = NIL_SORT;
            else if (item == Qtrue)  
                header[2] = TRUE_SORT;
            else if (item == Qfalse) 
                header[2] = FALSE_SORT;
            else    
                rb_raise(rb_eTypeError, "invalid type %s in tuple", rb_obj_classname(item));

            rb_str_cat(data, (char*)&header, sizeof(header));      
        }
    }
    return data;
}

static VALUE empty_bignum(int sign, int len) {
  return rb_big_new(len, sign);
}

static VALUE tuple_parse(void **data, int data_len) {
    VALUE tuple = rb_ary_new();
    VALUE item;
    void* ptr = *data; *data = &ptr;
    void* end = ptr + data_len;
    int i, len, sign;
    u_int8_t header[4];
    u_int32_t digit;

    while (ptr < end) {
        memcpy(header, ptr, 4);
        ptr += 4;

        switch(header[2]) {     // 数字
            case TRUE_SORT:  
                rb_ary_push(tuple, Qtrue);  
                break;
            case FALSE_SORT: 
                rb_ary_push(tuple, Qfalse); 
                break;
            case NIL_SORT:   
                rb_ary_push(tuple, Qnil);   
                break;
            case INTP_SORT:
            case INTN_SORT:
                sign  = (header[2] == INTP_SORT);
                len   = sign ? header[3] : (UCHAR_MAX - header[3]);

                unsigned char *buf = ALLOCA_N(unsigned char, len*4);
                memcpy(buf, ptr, len*4);
                ptr += len*4;

                for (size_t j = 0; j < len; j++) {
                    uint32_t v =
                        ((uint32_t)buf[j*4]   << 24) |
                        ((uint32_t)buf[j*4+1] << 16) |
                        ((uint32_t)buf[j*4+2] << 8) |
                        ((uint32_t)buf[j*4+3]);
                    if (!sign) v = UINT_MAX - v;
                    buf[j*4] = (v >> 24) & 0xFF;
                    buf[j*4+1] = (v >> 16) & 0xFF;
                    buf[j*4+2] = (v >> 8) & 0xFF;
                    buf[j*4+3] = v & 0xFF;
                }
                // 官方 API 生成 Bignum
                VALUE item = rb_integer_unpack(
                    (unsigned char*)buf,    // 转成 byte* 符合 API
                    len,                      // word 数
                    sizeof(uint32_t),         // 每个 word 4 字节
                    0,                        // unsigned
                    INTEGER_PACK_BIG_ENDIAN
                );

                // 负数处理
                if (!sign) 
                    item = rb_funcall(item, rb_intern("-@"), 0);
                
                rb_ary_push(tuple, item);
                break;
            case STR_SORT:
            case SYM_SORT:
                item = rb_str_new2(ptr);
                len  = RSTRING_LEN(item);
                if (header[2] == SYM_SORT) item = rb_funcall(item, rb_intern("to_sym"), 0);
                rb_ary_push(tuple, item);
                while (len % 4 != 0) len++; ptr += len;
                break;
            case TIME_SORT:
                item = rb_str_new2(ptr);
                len  = RSTRING_LEN(item);
                if (len == 10) 
                    item = rb_funcall(rb_cDate, rb_intern("parse"), 1, item);
                else           
                    item = rb_funcall(rb_cTime, rb_intern("parse"), 1, item);
                rb_ary_push(tuple, item);
                while (len % 4 != 0) len++; ptr += len;
                break;
            case TUPLE_SORT:
                item = tuple_parse(&ptr, end - ptr);
                rb_ary_push(tuple, item);
                break;
            case TUPLE_END:
                return tuple;
            default:
                rb_raise(rb_eTypeError, "invalid type code %d in tuple", header[2]);
                break;
        }
    }
    return tuple;
}

/*
 * call-seq:
 * Tuple.load(string) -> tuple
 *
 * Reads in a previously dumped tuple from a string of binary data.
 *
 */
static VALUE tuple_load(VALUE self, VALUE data) {
    data = StringValue(data);
    void* ptr = RSTRING_PTR(data);
    return tuple_parse(&ptr, RSTRING_LEN(data));
}

VALUE mTuple;
void Init_tuple() {
    rb_require("time");
    rb_require("date");
    rb_cDate = rb_const_get(rb_cObject, rb_intern("Date"));

    mTuple = rb_define_module("Tuple");
    rb_define_module_function(mTuple, "dump", tuple_dump, 1);
    rb_define_module_function(mTuple, "load", tuple_load, 1);
}

/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2023 Damiano Mazzella
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/nlr.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/objint.h"
#include "py/objtuple.h"
#include "py/objlist.h"
#include "py/objtype.h"
#include "py/parsenum.h"
#include "py/stream.h"
#include "py/runtime.h"

#include "tomsfastmath/tfm_mpi.h"

#define ERROR_ODD_LEN MP_ERROR_TEXT("odd-length string")
#define ERROR_NON_HEX MP_ERROR_TEXT("non-hex digit found")
#define ERROR_ODD_MODULUS MP_ERROR_TEXT("'exptmod' need odd modulus, set 'safe' or use 'fast_pow'")
#define ERROR_NUM_BITS MP_ERROR_TEXT("number of bits to generate must be in range 16-4096, not %lu bits")
#define ERROR_PRIME_LEN MP_ERROR_TEXT("Prime is %d, not %lu bits")
#define ERROR_EXPECTED_INT MP_ERROR_TEXT("expected a int, but %s found")
#define ERROR_EXPECTED_SIGNATURES MP_ERROR_TEXT("expected two Signature's")
#define ERROR_EXPECTED_CURVES MP_ERROR_TEXT("expected two Curve's")
#define ERROR_LEFT_EXPECTED_CURVE MP_ERROR_TEXT("left must be a Curve")
#define ERROR_RIGHT_EXPECTED_POINT MP_ERROR_TEXT("right must be a Point")
#define ERROR_EXPECTED_POINT_BUT MP_ERROR_TEXT("expected a Point, but %s found")
#define ERROR_EXPECTED_STR_BYTES_BUT MP_ERROR_TEXT("expected a str/bytes, but %s found")
#define ERROR_EXPECTED_POINT_AT_BUT MP_ERROR_TEXT("arg at index %d expected a Point, but %s found")
#define ERROR_EXPECTED_CURVE_AT_BUT MP_ERROR_TEXT("arg at index %d expected a Curve, but %s found")
#define ERROR_EXPECTED_INT_AT_BUT MP_ERROR_TEXT("arg at index %d expected a int, but %s found")
#define ERROR_EXPECTED_SIGNATURE_AT_BUT MP_ERROR_TEXT("arg at index %d expected a Signature, but %s found")
#define ERROR_EXPECTED_CURVE_BUT MP_ERROR_TEXT("expected a Curve, but %s found")
#define ERROR_EXPECTED_POINTS MP_ERROR_TEXT("expected two Point's")
#define ERROR_CURVE_OF_POINTS_NOT_EQUAL MP_ERROR_TEXT("curve of two Point's must be the same")
#define ERROR_LEFT_EXPECTED_POINT MP_ERROR_TEXT("left must be a Point")
#define ERROR_RIGHT_EXPECTED_INT MP_ERROR_TEXT("right must be a int")
#define ERROR_MEMORY MP_ERROR_TEXT("memory allocation failed, allocating %u bytes")

STATIC vstr_t *vstr_unhexlify(vstr_t *vstr_out, const byte *in, size_t in_len)
{
    if ((in_len & 1) != 0)
    {
        mp_raise_ValueError(ERROR_ODD_LEN);
    }

    vstr_init_len(vstr_out, in_len / 2);
    byte *out = (byte *)vstr_str(vstr_out);
    byte hex_byte = 0;
    for (mp_uint_t i = in_len; i--;)
    {
        byte hex_ch = *in++;
        if (unichar_isxdigit(hex_ch))
        {
            hex_byte += unichar_xdigit_value(hex_ch);
        }
        else
        {
            mp_raise_ValueError(ERROR_NON_HEX);
        }
        if (i & 1)
        {
            hex_byte <<= 4;
        }
        else
        {
            *out++ = hex_byte;
            hex_byte = 0;
        }
    }

    return vstr_out;
}

STATIC vstr_t *vstr_hexlify(vstr_t *vstr_out, const byte *in, size_t in_len)
{
    vstr_init(vstr_out, in_len);

    if (in != NULL && in_len)
    {
        for (mp_uint_t i = in_len; i--;)
        {
            byte d = (*in >> 4);
            if (d > 9)
            {
                d += 'a' - '9' - 1;
            }
            vstr_add_char(vstr_out, d + '0');
            d = (*in++ & 0xf);
            if (d > 9)
            {
                d += 'a' - '9' - 1;
            }
            vstr_add_char(vstr_out, d + '0');
        }
    }

    return vstr_out;
}

STATIC mp_obj_t fp_int_as_int(fp_int *b)
{
    mp_obj_int_t *o = mp_obj_int_new_mpz();
    mpz_init_zero(&o->mpz);

    if (fp_iszero(b))
    {
        return MP_OBJ_FROM_PTR(o);
    }

    size_t nbits = fp_count_bits(b);
    size_t bsize = (nbits >> 3) + (nbits & 7 ? 1 : 0);

    bool is_neg = false;
    if (fp_cmp_d(b, 0) == FP_LT)
    {
        is_neg = true;
    }

    if (is_neg)
    {
        fp_abs(b, b);
    }

    byte *bb = m_new(byte, bsize);
    if (bb == NULL)
    {
        mp_raise_msg_varg(&mp_type_MemoryError, ERROR_MEMORY, (uint)bsize);
    }

    fp_to_unsigned_bin(b, bb);

    mpz_set_from_bytes(&o->mpz, true, bsize, bb);

    m_free(bb);

    if (!mpz_is_zero(&o->mpz))
    {
        if (is_neg)
        {
            mpz_neg_inpl(&o->mpz, &o->mpz);
        }
    }

    return MP_OBJ_FROM_PTR(o);
}

STATIC size_t mpz_as_fp_int(mpz_t *i, fp_int *b)
{
    /* set the integer to the default of zero */
    fp_zero(b);

    if (mpz_is_zero(i))
    {
        return FP_OKAY;
    }

    size_t nbits = mpz_max_num_bits(i);
    size_t bsize = (nbits >> 3) + (nbits & 7 ? 1 : 0);

    byte *ib = m_new(byte, bsize);
    if (ib == NULL)
    {
        mp_raise_msg_varg(&mp_type_MemoryError, ERROR_MEMORY, (uint)bsize);
    }

    bool is_neg = mpz_is_neg(i);
    if (is_neg)
    {
        mpz_abs_inpl(i, i);
    }

    mpz_as_bytes(i, true, bsize, ib);

    if (is_neg)
    {
        mpz_neg_inpl(i, i);
    }

    fp_read_unsigned_bin(b, ib, bsize);

    m_free(ib);

    /* set the sign only if b != 0 */
    if (fp_iszero(b) != FP_YES)
    {
        if (is_neg)
        {
            b->sign = FP_NEG;
        }
        else
        {
            b->sign = FP_ZPOS;
        }
    }

    return FP_OKAY;
}

STATIC mpz_t *mp_mpz_for_int(mp_obj_t arg, mpz_t *temp)
{
    if (MP_OBJ_IS_SMALL_INT(arg))
    {
        mpz_init_from_int(temp, MP_OBJ_SMALL_INT_VALUE(arg));
        return temp;
    }
    else
    {
        mp_obj_int_t *arp_p = MP_OBJ_TO_PTR(arg);
        return &(arp_p->mpz);
    }
}

STATIC bool mp_fp_for_int(mp_obj_t arg, fp_int *ft_tmp)
{
    mpz_t arp_temp;
    fp_init(ft_tmp);
    mpz_t *arp_p = mp_mpz_for_int(arg, &arp_temp);
    mpz_as_fp_int(arp_p, ft_tmp);
    if (arp_p == &arp_temp)
    {
        mpz_deinit(arp_p);
    }
    return true;
}

#if 0
STATIC vstr_t *vstr_new_from_mpz(const mpz_t *i)
{
    size_t len = mp_int_format_size(mpz_max_num_bits(i), 10, NULL, '\0');
    vstr_t *vstr = vstr_new(len);
    mpz_as_str_inpl(i, 10, NULL, 'a', '\0', vstr_str(vstr));
    vstr->len = len;
    return vstr;
}
#endif

STATIC vstr_t *vstr_new_from_fp(const fp_int *fp)
{
    size_t size_fp;
    fp_radix_size((fp_int *)fp, 10, (int *)&size_fp);
    vstr_t *vstr_fp = vstr_new(size_fp);
    vstr_fp->len = size_fp;
    fp_toradix_n((fp_int *)fp, vstr_str(vstr_fp), 10, vstr_len(vstr_fp));
    return vstr_fp;
}

STATIC mp_obj_t mp_obj_new_int_from_fp(fp_int *fp)
{
    return fp_int_as_int(fp);
}

/* returns a TFM ident string useful for debugging... */
STATIC mp_obj_t mod_ident(void)
{
    const char *s = fp_ident();
    size_t s_len = strlen(s);
    vstr_t vstr_out;
    vstr_init(&vstr_out, s_len);
    vstr_add_strn(&vstr_out, s, s_len);
    return mp_obj_new_str_from_vstr(&vstr_out);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_ident_obj, mod_ident);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(mod_static_ident_obj, MP_ROM_PTR(&mod_ident_obj));

static int fp_pow3(fp_int *X, fp_int *E, fp_int *M, fp_int *Y)
{
    fp_int x;
    fp_int e;
    fp_int y;

    fp_init(&x);
    fp_init(&e);
    fp_init(&y);

    fp_copy(X, &x);
    fp_copy(E, &e);
    fp_set(&y, 1);

    // while E > 0:
    while (fp_cmp_d(&e, 0) == FP_GT)
    {
        // if E % 2 == 0:
        fp_digit Emod2;
        fp_mod_d(&e, 2, &Emod2);
        if (Emod2 == 0)
        {
            // X = (X * X) % m
            fp_sqrmod(&x, M, &x);
            // E = E / 2
            fp_div_2(&e, &e);
        }
        else
        {
            // Y = (X * Y) % m
            fp_mulmod(&x, &y, M, &y);
            // E = E - 1
            fp_sub_d(&e, 1, &e);
        }
    }

    fp_copy(&y, Y);

    return FP_OKAY;
}

STATIC mp_obj_t mod_fast_pow(mp_obj_t A_in, mp_obj_t B_in, mp_obj_t C_in)
{
    fp_int a_fp_int, b_fp_int, c_fp_int, d_fp_int;
    fp_init(&d_fp_int);
    mp_fp_for_int(A_in, &a_fp_int);
    mp_fp_for_int(B_in, &b_fp_int);
    mp_fp_for_int(C_in, &c_fp_int);

    fp_pow3(&a_fp_int, &b_fp_int, &c_fp_int, &d_fp_int);

    return mp_obj_new_int_from_fp(&d_fp_int);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_fast_pow_obj, mod_fast_pow);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(mod_static_fast_pow_obj, MP_ROM_PTR(&mod_fast_pow_obj));

/* d = a**b (mod c) */
STATIC mp_obj_t mod_exptmod(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_a, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_b, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_c, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_safe, MP_ARG_BOOL, {.u_bool = false}},
    };

    struct
    {
        mp_arg_val_t a, b, c, safe;
    } args;
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, (mp_arg_val_t *)&args);

    fp_int a_fp_int, b_fp_int, c_fp_int, d_fp_int;
    fp_init(&d_fp_int);
    mp_fp_for_int(args.a.u_obj, &a_fp_int);
    mp_fp_for_int(args.b.u_obj, &b_fp_int);
    mp_fp_for_int(args.c.u_obj, &c_fp_int);

    // the montgomery reduce need odd modulus
    if (fp_isodd(&c_fp_int) == FP_YES)
    {
        fp_exptmod(&a_fp_int, &b_fp_int, &c_fp_int, &d_fp_int);
    }
    else
    {
        if (args.safe.u_bool)
        {
            fp_pow3(&a_fp_int, &b_fp_int, &c_fp_int, &d_fp_int);
        }
        else
        {
            mp_raise_ValueError(ERROR_ODD_MODULUS);
        }
    }

    return mp_obj_new_int_from_fp(&d_fp_int);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_exptmod_obj, 3, mod_exptmod);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(mod_static_exptmod_obj, MP_ROM_PTR(&mod_exptmod_obj));

/* c = 1/a (mod b) */
STATIC mp_obj_t mod_invmod(mp_obj_t A_in, mp_obj_t B_in)
{
    fp_int a_fp_int, b_fp_int, c_fp_int;
    fp_init(&c_fp_int);
    mp_fp_for_int(A_in, &a_fp_int);
    mp_fp_for_int(B_in, &b_fp_int);
    fp_invmod(&a_fp_int, &b_fp_int, &c_fp_int);
    return mp_obj_new_int_from_fp(&c_fp_int);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_invmod_obj, mod_invmod);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(mod_static_invmod_obj, MP_ROM_PTR(&mod_invmod_obj));

/* c = (a, b) */
STATIC mp_obj_t mod_gcd(mp_obj_t A_in, mp_obj_t B_in)
{
    fp_int a_fp_int, b_fp_int, c_fp_int;
    fp_init(&c_fp_int);
    mp_fp_for_int(A_in, &a_fp_int);
    mp_fp_for_int(B_in, &b_fp_int);
    fp_gcd(&a_fp_int, &b_fp_int, &c_fp_int);
    return mp_obj_new_int_from_fp(&c_fp_int);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_gcd_obj, mod_gcd);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(mod_static_gcd_obj, MP_ROM_PTR(&mod_gcd_obj));

STATIC int ucrypto_rng(unsigned char *dst, int len, void *dat)
{
    (void)dat;
    for (int x = 0; x < len; x++)
    {
        dst[x] = FP_GEN_RANDOM();
    }
    return len;
}

/* generate prime number */

#if defined(__thumb2__) || defined(__thumb__) || defined(__arm__)
#if !defined(malloc) && !defined(free)
void *malloc(size_t n)
{
    void *ptr = m_malloc(n);
    return ptr;
}

void free(void *ptr)
{
    m_free(ptr);
}
#endif
#endif

STATIC mp_obj_t mod_generate_prime(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_num, MP_ARG_INT, {.u_int = 1024}},
        {MP_QSTR_test, MP_ARG_INT, {.u_int = 25}},
        {MP_QSTR_safe, MP_ARG_BOOL, {.u_bool = false}}};

    struct
    {
        mp_arg_val_t num, test, safe;
    } args;
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, (mp_arg_val_t *)&args);

    if (args.num.u_int < 16 || args.num.u_int > 4096)
    {
        mp_raise_msg_varg(&mp_type_ValueError, ERROR_NUM_BITS, args.num.u_int);
    }
    int flags = ((FP_GEN_RANDOM() & 1) ? TFM_PRIME_2MSB_OFF : TFM_PRIME_2MSB_ON);
    if (args.safe.u_bool)
    {
        flags |= TFM_PRIME_SAFE;
    }
    fp_int a_fp_int;
    int ret = FP_OKAY;
    if ((ret = fp_prime_random_ex(&a_fp_int, args.test.u_int, args.num.u_int, flags, ucrypto_rng, NULL)) == FP_OKAY)
    {
        if (fp_count_bits(&a_fp_int) != args.num.u_int)
        {
            mp_raise_msg_varg(&mp_type_ValueError, ERROR_PRIME_LEN, fp_count_bits(&a_fp_int), args.num.u_int);
        }
    }
    else
    {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%d"), ret);
    }
    return mp_obj_new_int_from_fp(&a_fp_int);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_generate_prime_obj, 1, mod_generate_prime);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(mod_static_generate_prime_obj, MP_ROM_PTR(&mod_generate_prime_obj));

STATIC mp_obj_t mod_is_prime(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_a, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_test, MP_ARG_INT, {.u_int = 25}},
    };

    struct
    {
        mp_arg_val_t a, test;
    } args;
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, (mp_arg_val_t *)&args);
    if (!MP_OBJ_IS_INT(args.a.u_obj))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT, mp_obj_get_type_str(args.a.u_obj));
    }
    fp_int a_fp_int;
    mp_fp_for_int(args.a.u_obj, &a_fp_int);
    int ret = fp_isprime_ex(&a_fp_int, args.test.u_int);
    return mp_obj_new_bool(ret);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_is_prime_obj, 1, mod_is_prime);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(mod_static_is_prime_obj, MP_ROM_PTR(&mod_is_prime_obj));

STATIC void number_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_printf(print, mp_obj_get_type_str(self_in));
}

STATIC const mp_rom_map_elem_t number_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_ident), MP_ROM_PTR(&mod_static_ident_obj)},
    {MP_ROM_QSTR(MP_QSTR_exptmod), MP_ROM_PTR(&mod_static_exptmod_obj)},
    {MP_ROM_QSTR(MP_QSTR_fast_pow), MP_ROM_PTR(&mod_static_fast_pow_obj)},
    {MP_ROM_QSTR(MP_QSTR_invmod), MP_ROM_PTR(&mod_static_invmod_obj)},
    {MP_ROM_QSTR(MP_QSTR_gcd), MP_ROM_PTR(&mod_static_gcd_obj)},
    {MP_ROM_QSTR(MP_QSTR_generate_prime), MP_ROM_PTR(&mod_static_generate_prime_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_prime), MP_ROM_PTR(&mod_static_is_prime_obj)},
};

STATIC MP_DEFINE_CONST_DICT(number_locals_dict, number_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    number_type,
    MP_QSTR_NUMBER,
    MP_TYPE_FLAG_NONE,
    print, number_print,
    locals_dict, &number_locals_dict);

// point in a prime field
typedef struct _ecc_point_t
{
    fp_int x;
    fp_int y;
} ecc_point_t;

// curve over a prime field
typedef struct _ecc_curve_t
{
    fp_int p;
    fp_int a;
    fp_int b;
    fp_int q;
    ecc_point_t g;
    vstr_t name;
    vstr_t oid;
} ecc_curve_t;

typedef struct _ecdsa_signature_t
{
    fp_int r;
    fp_int s;
} ecdsa_signature_t;

typedef struct _mp_curve_t
{
    mp_obj_base_t base;
    ecc_curve_t ecc_curve;
} mp_curve_t;

typedef struct _mp_point_t
{
    mp_obj_base_t base;
    ecc_point_t ecc_point;
    ecc_curve_t ecc_curve;
} mp_point_t;

typedef struct _mp_ecdsa_signature_t
{
    mp_obj_base_t base;
    ecdsa_signature_t ecdsa_signature;
} mp_ecdsa_signature_t;

const mp_obj_type_t signature_type;
const mp_obj_type_t curve_type;
const mp_obj_type_t point_type;
const mp_obj_type_t ecc_type;

STATIC mp_curve_t *new_curve_init_copy(mp_point_t *point)
{
    mp_curve_t *c = m_new_obj(mp_curve_t);
    c->base.type = &curve_type;
    vstr_init(&c->ecc_curve.name, vstr_len(&point->ecc_curve.oid));
    vstr_add_strn(&c->ecc_curve.name, vstr_str(&point->ecc_curve.name), vstr_len(&point->ecc_curve.name));
    vstr_init(&c->ecc_curve.oid, vstr_len(&point->ecc_curve.oid));
    vstr_add_strn(&c->ecc_curve.oid, vstr_str(&point->ecc_curve.oid), vstr_len(&point->ecc_curve.oid));
    fp_copy(&point->ecc_curve.p, &c->ecc_curve.p);
    fp_copy(&point->ecc_curve.a, &c->ecc_curve.a);
    fp_copy(&point->ecc_curve.b, &c->ecc_curve.b);
    fp_copy(&point->ecc_curve.q, &c->ecc_curve.q);
    fp_copy(&point->ecc_curve.g.x, &c->ecc_curve.g.x);
    fp_copy(&point->ecc_curve.g.y, &c->ecc_curve.g.y);
    return c;
}

STATIC mp_point_t *new_point_init_copy(mp_curve_t *curve)
{
    mp_point_t *pr = m_new_obj(mp_point_t);
    pr->base.type = &point_type;
    vstr_init(&pr->ecc_curve.name, vstr_len(&curve->ecc_curve.oid));
    vstr_add_strn(&pr->ecc_curve.name, vstr_str(&curve->ecc_curve.name), vstr_len(&curve->ecc_curve.name));
    vstr_init(&pr->ecc_curve.oid, vstr_len(&curve->ecc_curve.oid));
    vstr_add_strn(&pr->ecc_curve.oid, vstr_str(&curve->ecc_curve.oid), vstr_len(&curve->ecc_curve.oid));
    fp_copy(&curve->ecc_curve.p, &pr->ecc_curve.p);
    fp_copy(&curve->ecc_curve.a, &pr->ecc_curve.a);
    fp_copy(&curve->ecc_curve.b, &pr->ecc_curve.b);
    fp_copy(&curve->ecc_curve.q, &pr->ecc_curve.q);
    fp_copy(&curve->ecc_curve.g.x, &pr->ecc_curve.g.x);
    fp_copy(&curve->ecc_curve.g.y, &pr->ecc_curve.g.y);
    fp_copy(&curve->ecc_curve.g.x, &pr->ecc_point.x);
    fp_copy(&curve->ecc_curve.g.y, &pr->ecc_point.y);
    return pr;
}

STATIC bool ec_signature_equal(const ecdsa_signature_t *s1, const ecdsa_signature_t *s2)
{
    if (fp_cmp((fp_int *)&s1->r, (fp_int *)&s2->r) != FP_EQ)
    {
        return false;
    }
    if (fp_cmp((fp_int *)&s1->s, (fp_int *)&s2->s) != FP_EQ)
    {
        return false;
    }
    return true;
}

STATIC void signature_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_ecdsa_signature_t *self = MP_OBJ_TO_PTR(self_in);
    vstr_t *vstr_r = vstr_new_from_fp((fp_int *)&self->ecdsa_signature.r);
    vstr_t *vstr_s = vstr_new_from_fp((fp_int *)&self->ecdsa_signature.s);
    mp_printf(print, "<Signature r=%s s=%s>", vstr_str(vstr_r), vstr_str(vstr_s));
    vstr_free(vstr_r);
    vstr_free(vstr_s);
}

STATIC mp_obj_t signature_binary_op(mp_binary_op_t op, mp_obj_t lhs, mp_obj_t rhs)
{
    switch (op)
    {
    case MP_BINARY_OP_EQUAL:
    {
        if (!MP_OBJ_IS_TYPE(lhs, &signature_type) && !MP_OBJ_IS_TYPE(rhs, &signature_type))
        {
            mp_raise_TypeError(ERROR_EXPECTED_SIGNATURES);
        }
        mp_ecdsa_signature_t *l = MP_OBJ_TO_PTR(lhs);
        mp_ecdsa_signature_t *r = MP_OBJ_TO_PTR(rhs);
        return mp_obj_new_bool(ec_signature_equal(&l->ecdsa_signature, &r->ecdsa_signature));
    }
    default:
        return MP_OBJ_NULL; // op not supported
    }
}

STATIC void signature_attr(mp_obj_t obj, qstr attr, mp_obj_t *dest)
{
    mp_ecdsa_signature_t *self = MP_OBJ_TO_PTR(obj);
    if (dest[0] == MP_OBJ_NULL)
    {
        const mp_obj_type_t *type = mp_obj_get_type(obj);
        mp_map_t *locals_map = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
        mp_map_elem_t *elem = mp_map_lookup(locals_map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
        if (elem != NULL)
        {
            if (attr == MP_QSTR_r)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecdsa_signature.r);
                return;
            }
            else if (attr == MP_QSTR_s)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecdsa_signature.s);
                return;
            }
            mp_convert_member_lookup(obj, type, elem->value, dest);
        }
    }
}

STATIC const mp_rom_map_elem_t signature_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_s), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_r), MP_ROM_INT(0)},
};

STATIC MP_DEFINE_CONST_DICT(signature_locals_dict, signature_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    signature_type,
    MP_QSTR_Signature,
    MP_TYPE_FLAG_NONE,
    print, signature_print,
    binary_op, signature_binary_op,
    attr, signature_attr,
    locals_dict, &signature_locals_dict);

STATIC bool ec_point_in_curve(const ecc_point_t *point, const ecc_curve_t *curve)
{
    int is_point_in_curve = 0;
    fp_int x;
    fp_int y;
    fp_int left;
    fp_int right;
    fp_int x_mul_x_mul_x;
    fp_int curvea_mul_x;
    fp_int left_minus_right_mod_curvep;

    fp_init(&x);
    fp_init(&y);
    fp_init(&left);
    fp_init(&x_mul_x_mul_x);
    fp_init(&curvea_mul_x);
    fp_init(&right);
    fp_init(&left_minus_right_mod_curvep);

    fp_copy(&point->x, &x);
    fp_copy(&point->y, &y);

    /*
    left = y * y
    right = (x * x * x) + (self.a * x) + self.b
    return (left - right) % self.p == 0
    */

    // left = (y * y)
    fp_sqr(&y, &left);

    //(x * x * x)
    fp_mul(&x, &x, &x_mul_x_mul_x);
    fp_mul(&x_mul_x_mul_x, &x, &x_mul_x_mul_x);

    //(curve.a * x)
    fp_mul((fp_int *)&curve->a, &x, &curvea_mul_x);

    // right = (x * x * x) + (curve.a * x) + curve.b
    fp_add(&x_mul_x_mul_x, &curvea_mul_x, &right);
    fp_add(&right, (fp_int *)&curve->b, &right);

    // return (left - right) % curve.p == 0
    fp_submod(&left, &right, (fp_int *)&curve->p, &left_minus_right_mod_curvep);
    is_point_in_curve = (fp_iszero(&left_minus_right_mod_curvep) == FP_YES);
    return is_point_in_curve;
}

STATIC bool ec_curve_equal(const ecc_curve_t *c1, const ecc_curve_t *c2)
{
    if (fp_cmp((fp_int *)&c1->p, (fp_int *)&c2->p) != FP_EQ)
    {
        return false;
    }
    if (fp_cmp((fp_int *)&c1->a, (fp_int *)&c2->a) != FP_EQ)
    {
        return false;
    }
    if (fp_cmp((fp_int *)&c1->b, (fp_int *)&c2->b) != FP_EQ)
    {
        return false;
    }
    if (fp_cmp((fp_int *)&c1->q, (fp_int *)&c2->q) != FP_EQ)
    {
        return false;
    }
    if (fp_cmp((fp_int *)&c1->g.x, (fp_int *)&c2->g.x) != FP_EQ)
    {
        return false;
    }
    if (fp_cmp((fp_int *)&c1->g.y, (fp_int *)&c2->g.y) != FP_EQ)
    {
        return false;
    }
    return true;
}

STATIC mp_obj_t curve_equal(mp_obj_t curve1, mp_obj_t curve2)
{
    if (!MP_OBJ_IS_TYPE(curve1, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 1, mp_obj_get_type_str(curve1));
    }
    if (!MP_OBJ_IS_TYPE(curve2, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 2, mp_obj_get_type_str(curve2));
    }

    mp_curve_t *c1 = MP_OBJ_TO_PTR(curve1);
    mp_curve_t *c2 = MP_OBJ_TO_PTR(curve2);
    return mp_obj_new_bool(ec_curve_equal(&c1->ecc_curve, &c2->ecc_curve));
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(curve_equal_obj, curve_equal);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_curve_equal_obj, MP_ROM_PTR(&curve_equal_obj));

STATIC mp_obj_t point_in_curve(mp_obj_t point, mp_obj_t curve)
{
    if (!MP_OBJ_IS_TYPE(point, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 1, mp_obj_get_type_str(point));
    }
    if (!MP_OBJ_IS_TYPE(curve, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 2, mp_obj_get_type_str(curve));
    }

    mp_point_t *p = MP_OBJ_TO_PTR(point);
    mp_curve_t *c = MP_OBJ_TO_PTR(curve);
    return mp_obj_new_bool(ec_point_in_curve(&p->ecc_point, &c->ecc_curve));
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(point_in_curve_obj, point_in_curve);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_point_in_curve_obj, MP_ROM_PTR(&point_in_curve_obj));

STATIC void curve_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_curve_t *self = MP_OBJ_TO_PTR(self_in);
    vstr_t vstr_oid;
    vstr_hexlify(&vstr_oid, (const byte *)vstr_str(&self->ecc_curve.oid), vstr_len(&self->ecc_curve.oid));
    vstr_t *ecc_curve_p = vstr_new_from_fp((fp_int *)&self->ecc_curve.p);
    vstr_t *ecc_curve_a = vstr_new_from_fp((fp_int *)&self->ecc_curve.a);
    vstr_t *ecc_curve_b = vstr_new_from_fp((fp_int *)&self->ecc_curve.b);
    vstr_t *ecc_curve_q = vstr_new_from_fp((fp_int *)&self->ecc_curve.q);
    vstr_t *ecc_curve_g_x = vstr_new_from_fp((fp_int *)&self->ecc_curve.g.x);
    vstr_t *ecc_curve_g_y = vstr_new_from_fp((fp_int *)&self->ecc_curve.g.y);
    mp_printf(print, "<Curve name=%s oid=%s p=%s a=%s b=%s q=%s gx=%s gy=%s>", vstr_str(&self->ecc_curve.name), vstr_str(&vstr_oid), vstr_str(ecc_curve_p), vstr_str(ecc_curve_a), vstr_str(ecc_curve_b), vstr_str(ecc_curve_q), vstr_str(ecc_curve_g_x), vstr_str(ecc_curve_g_y));
    vstr_free(ecc_curve_p);
    vstr_free(ecc_curve_a);
    vstr_free(ecc_curve_b);
    vstr_free(ecc_curve_q);
    vstr_free(ecc_curve_g_x);
    vstr_free(ecc_curve_g_y);
}

STATIC mp_obj_t curve_binary_op(mp_binary_op_t op, mp_obj_t lhs, mp_obj_t rhs)
{
    switch (op)
    {
    case MP_BINARY_OP_EQUAL:
    {
        if (!MP_OBJ_IS_TYPE(lhs, &curve_type) && !MP_OBJ_IS_TYPE(rhs, &curve_type))
        {
            mp_raise_TypeError(ERROR_EXPECTED_CURVES);
        }
        mp_curve_t *l = MP_OBJ_TO_PTR(lhs);
        mp_curve_t *r = MP_OBJ_TO_PTR(rhs);
        return curve_equal((mp_obj_t)l, (mp_obj_t)r);
    }
    case MP_BINARY_OP_CONTAINS:
    {
        if (!MP_OBJ_IS_TYPE(lhs, &curve_type))
        {
            mp_raise_TypeError(ERROR_LEFT_EXPECTED_CURVE);
        }
        if (!MP_OBJ_IS_TYPE(rhs, &point_type))
        {
            mp_raise_TypeError(ERROR_RIGHT_EXPECTED_POINT);
        }
        mp_curve_t *l = MP_OBJ_TO_PTR(lhs);
        mp_point_t *r = MP_OBJ_TO_PTR(rhs);
        return point_in_curve((mp_obj_t)r, (mp_obj_t)l);
    }
    default:
        return MP_OBJ_NULL; // op not supported
    }
}

STATIC void curve_attr(mp_obj_t obj, qstr attr, mp_obj_t *dest)
{
    mp_curve_t *self = MP_OBJ_TO_PTR(obj);
    if (dest[0] == MP_OBJ_NULL)
    {
        const mp_obj_type_t *type = mp_obj_get_type(obj);
        mp_map_t *locals_map = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
        mp_map_elem_t *elem = mp_map_lookup(locals_map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
        if (elem != NULL)
        {
            if (attr == MP_QSTR_p)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_curve.p);
                return;
            }
            else if (attr == MP_QSTR_a)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_curve.a);
                return;
            }
            else if (attr == MP_QSTR_b)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_curve.b);
                return;
            }
            else if (attr == MP_QSTR_q)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_curve.q);
                return;
            }
            else if (attr == MP_QSTR_G)
            {
                mp_point_t *pr = new_point_init_copy(self);
                dest[0] = pr;
                return;
            }
            else if (attr == MP_QSTR_gx)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_curve.g.x);
                return;
            }
            else if (attr == MP_QSTR_gy)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_curve.g.y);
                return;
            }
            else if (attr == MP_QSTR_name)
            {
                dest[0] = mp_obj_new_str_of_type(&mp_type_str, (const byte *)vstr_str(&self->ecc_curve.name), vstr_len(&self->ecc_curve.name));
                return;
            }
            else if (attr == MP_QSTR_oid)
            {
                dest[0] = mp_obj_new_str_of_type(&mp_type_bytes, (const byte *)vstr_str(&self->ecc_curve.oid), vstr_len(&self->ecc_curve.oid));
                return;
            }
            mp_convert_member_lookup(obj, type, elem->value, dest);
        }
    }
    else
    {
        if ((attr == MP_QSTR_p || attr == MP_QSTR_a || attr == MP_QSTR_b || attr == MP_QSTR_q || attr == MP_QSTR_gx || attr == MP_QSTR_gy) && !MP_OBJ_IS_INT(dest[1]))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT, mp_obj_get_type_str(dest[1]));
        }
        else if (attr == MP_QSTR_G && !MP_OBJ_IS_TYPE(dest[1], &point_type))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_BUT, mp_obj_get_type_str(dest[1]));
        }
        else if ((attr == MP_QSTR_name || attr == MP_QSTR_oid) && !MP_OBJ_IS_STR_OR_BYTES(dest[1]))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_STR_BYTES_BUT, mp_obj_get_type_str(dest[1]));
        }

        if (attr == MP_QSTR_p)
        {
            mp_fp_for_int(dest[1], &self->ecc_curve.p);
        }
        else if (attr == MP_QSTR_a)
        {
            mp_fp_for_int(dest[1], &self->ecc_curve.a);
        }
        else if (attr == MP_QSTR_b)
        {
            mp_fp_for_int(dest[1], &self->ecc_curve.b);
        }
        else if (attr == MP_QSTR_q)
        {
            mp_fp_for_int(dest[1], &self->ecc_curve.q);
        }
        else if (attr == MP_QSTR_G)
        {
            mp_point_t *other = MP_OBJ_TO_PTR(dest[1]);
            fp_copy(&other->ecc_curve.p, &self->ecc_curve.p);
            fp_copy(&other->ecc_curve.a, &self->ecc_curve.a);
            fp_copy(&other->ecc_curve.b, &self->ecc_curve.b);
            fp_copy(&other->ecc_curve.q, &self->ecc_curve.q);
            fp_copy(&other->ecc_point.x, &self->ecc_curve.g.x);
            fp_copy(&other->ecc_point.y, &self->ecc_curve.g.y);
        }
        else if (attr == MP_QSTR_gx)
        {
            mp_fp_for_int(dest[1], &self->ecc_curve.g.x);
        }
        else if (attr == MP_QSTR_gy)
        {
            mp_fp_for_int(dest[1], &self->ecc_curve.g.y);
        }
        else if (attr == MP_QSTR_name)
        {
            mp_buffer_info_t bufinfo_name;
            mp_get_buffer_raise(dest[1], &bufinfo_name, MP_BUFFER_READ);
            vstr_init(&self->ecc_curve.name, bufinfo_name.len);
            vstr_add_strn(&self->ecc_curve.name, bufinfo_name.buf, bufinfo_name.len);
        }
        else if (attr == MP_QSTR_oid)
        {
            mp_buffer_info_t bufinfo_oid;
            mp_get_buffer_raise(dest[1], &bufinfo_oid, MP_BUFFER_READ);

            if (MP_OBJ_IS_TYPE(dest[1], &mp_type_bytes))
            {
                vstr_init(&self->ecc_curve.oid, bufinfo_oid.len);
                vstr_add_strn(&self->ecc_curve.oid, bufinfo_oid.buf, bufinfo_oid.len);
            }
            else if (MP_OBJ_IS_STR(dest[1]))
            {
                vstr_unhexlify(&self->ecc_curve.oid, bufinfo_oid.buf, bufinfo_oid.len);
            }
        }
        else
        {
            return;
        }
        dest[0] = MP_OBJ_NULL; // indicate success
    }
}

STATIC const mp_rom_map_elem_t curve_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_p), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_a), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_b), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_q), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_G), MP_ROM_PTR(mp_const_none)},
    {MP_ROM_QSTR(MP_QSTR_gx), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_gy), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_name), MP_ROM_PTR(mp_const_none)},
    {MP_ROM_QSTR(MP_QSTR_oid), MP_ROM_PTR(mp_const_none)},
};

STATIC MP_DEFINE_CONST_DICT(curve_locals_dict, curve_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    curve_type,
    MP_QSTR_Curve,
    MP_TYPE_FLAG_NONE,
    print, curve_print,
    binary_op, curve_binary_op,
    attr, curve_attr,
    locals_dict, &curve_locals_dict);

STATIC mp_obj_t curve(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    /*
        Currently only curves in Weierstrass form (y^2 = x^3 + ax + b (mod p))

        p (int): The value of math p in the curve equation
        a (int): The value of math a in the curve equation
        b (int): The value of math b in the curve equation
        q (int): The order of the base point of the curve
        gx (int): The x coordinate of the base point of the curve
        gy (int): The y coordinate of the base point of the curve
        name (str): The name of the curve
        oid (str/bytes): The object identifier of the curve
    */

    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_p, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_a, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_b, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_q, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_gx, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_gy, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_name, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_oid, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    };

    struct
    {
        mp_arg_val_t p, a, b, q, gx, gy, name, oid;
    } args;
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, (mp_arg_val_t *)&args);

    mp_curve_t *curve = m_new_obj(mp_curve_t);
    curve->base.type = &curve_type;
    vstr_init(&curve->ecc_curve.name, 0);
    vstr_init(&curve->ecc_curve.oid, 0);
    for (size_t i = 0; i < n_args; i++)
    {
        if (!MP_OBJ_IS_INT(pos_args[i]))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT_AT_BUT, mp_obj_get_type_str(pos_args[i]), i);
        }
    }

    mp_fp_for_int(args.p.u_obj, &curve->ecc_curve.p);
    mp_fp_for_int(args.a.u_obj, &curve->ecc_curve.a);
    mp_fp_for_int(args.b.u_obj, &curve->ecc_curve.b);
    mp_fp_for_int(args.q.u_obj, &curve->ecc_curve.q);
    mp_fp_for_int(args.gx.u_obj, &curve->ecc_curve.g.x);
    mp_fp_for_int(args.gy.u_obj, &curve->ecc_curve.g.y);

    if (args.name.u_obj != mp_const_none)
    {
        if (!MP_OBJ_IS_STR_OR_BYTES(args.name.u_obj))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_STR_BYTES_BUT, mp_obj_get_type_str(args.name.u_obj));
        }
        else
        {
            mp_buffer_info_t bufinfo_name;
            mp_get_buffer_raise(args.name.u_obj, &bufinfo_name, MP_BUFFER_READ);
            vstr_add_strn(&curve->ecc_curve.name, bufinfo_name.buf, bufinfo_name.len);
        }
    }

    if (args.oid.u_obj != mp_const_none)
    {
        if (!MP_OBJ_IS_STR_OR_BYTES(args.oid.u_obj))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_STR_BYTES_BUT, mp_obj_get_type_str(args.oid.u_obj));
        }
        else
        {
            mp_buffer_info_t bufinfo_oid;
            mp_get_buffer_raise(args.oid.u_obj, &bufinfo_oid, MP_BUFFER_READ);

            if (MP_OBJ_IS_TYPE(args.oid.u_obj, &mp_type_bytes))
            {
                vstr_init(&curve->ecc_curve.oid, bufinfo_oid.len);
                vstr_add_strn(&curve->ecc_curve.oid, bufinfo_oid.buf, bufinfo_oid.len);
            }
            else if (MP_OBJ_IS_STR(args.oid.u_obj))
            {
                vstr_unhexlify(&curve->ecc_curve.oid, bufinfo_oid.buf, bufinfo_oid.len);
            }
        }
    }

    return (mp_obj_t)curve;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(curve_obj, 6, curve);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_curve_obj, MP_ROM_PTR(&curve_obj));

///////////////////////////////////// Point ////////////////////////////////////

STATIC bool ec_point_equal(const ecc_point_t *p1, const ecc_point_t *p2)
{
    // check x coords
    if (fp_cmp((fp_int *)&p1->x, (fp_int *)&p2->x) != FP_EQ)
    {
        return false;
    }
    // check y coords
    if (fp_cmp((fp_int *)&p1->y, (fp_int *)&p2->y) != FP_EQ)
    {
        return false;
    }
    return true;
}

STATIC void ec_point_double(const ecc_point_t *rop, const ecc_point_t *op, const ecc_curve_t *curve)
{
    if (fp_cmp_d((fp_int *)&op->x, 0) == FP_EQ && fp_cmp_d((fp_int *)&op->y, 0) == FP_EQ)
    {
        fp_set((fp_int *)&rop->x, 0);
        fp_set((fp_int *)&rop->y, 0);
        return;
    }

    fp_int numer, denom, lambda;
    fp_init(&numer);
    fp_init(&denom);
    fp_init(&lambda);

    // calculate lambda
    fp_mul((fp_int *)&op->x, (fp_int *)&op->x, &numer);
    fp_mul_d(&numer, 3, &numer);
    fp_add(&numer, (fp_int *)&curve->a, &numer);
    fp_mul_d((fp_int *)&op->y, 2, &denom);

    // handle 2P = identity case
    if (fp_invmod(&denom, (fp_int *)&curve->p, &denom) != FP_OKAY)
    {
        fp_set((fp_int *)&rop->x, 0);
        fp_set((fp_int *)&rop->y, 0);
        return;
    }

    fp_mul(&numer, &denom, &lambda);
    fp_mod(&lambda, (fp_int *)&curve->p, &lambda);

    // calculate resulting x coord
    fp_mul(&lambda, &lambda, (fp_int *)&rop->x);
    fp_sub((fp_int *)&rop->x, (fp_int *)&op->x, (fp_int *)&rop->x);
    fp_sub((fp_int *)&rop->x, (fp_int *)&op->x, (fp_int *)&rop->x);
    fp_mod((fp_int *)&rop->x, (fp_int *)&curve->p, (fp_int *)&rop->x);

    // calculate resulting y coord
    fp_sub((fp_int *)&op->x, (fp_int *)&rop->x, (fp_int *)&rop->y);
    fp_mul(&lambda, (fp_int *)&rop->y, (fp_int *)&rop->y);
    fp_sub((fp_int *)&rop->y, (fp_int *)&op->y, (fp_int *)&rop->y);
    fp_mod((fp_int *)&rop->y, (fp_int *)&curve->p, (fp_int *)&rop->y);
}

STATIC void ec_point_add(ecc_point_t *rop, const ecc_point_t *op1, const ecc_point_t *op2, const ecc_curve_t *curve)
{
    // handle the identity element
    if (fp_cmp_d((fp_int *)&op1->x, 0) == FP_EQ && fp_cmp_d((fp_int *)&op1->y, 0) == FP_EQ && fp_cmp_d((fp_int *)&op2->x, 0) == FP_EQ && fp_cmp_d((fp_int *)&op2->y, 0) == FP_EQ)
    {
        fp_set((fp_int *)&rop->x, 0);
        fp_set((fp_int *)&rop->y, 0);
        return;
    }
    else if (fp_cmp_d((fp_int *)&op1->x, 0) == FP_EQ && fp_cmp_d((fp_int *)&op1->y, 0) == FP_EQ)
    {
        fp_copy((fp_int *)&op2->x, &rop->x);
        fp_copy((fp_int *)&op2->y, &rop->y);
        return;
    }
    else if (fp_cmp_d((fp_int *)&op2->x, 0) == FP_EQ && fp_cmp_d((fp_int *)&op2->y, 0) == FP_EQ)
    {
        fp_copy((fp_int *)&op1->x, &rop->x);
        fp_copy((fp_int *)&op1->y, &rop->y);
        return;
    }

    if (ec_point_equal(op1, op2))
    {
        ec_point_double(rop, op1, curve);
        return;
    }

    // check if points sum to identity element
    fp_int negy;
    fp_init(&negy);
    fp_sub((fp_int *)&curve->p, (fp_int *)&op2->y, &negy);
    if (fp_cmp((fp_int *)&op1->x, (fp_int *)&op2->x) == FP_EQ && fp_cmp((fp_int *)&op1->y, &negy) == 0)
    {
        fp_set((fp_int *)&rop->x, 0);
        fp_set((fp_int *)&rop->y, 0);
        return;
    }

    fp_int xdiff, ydiff, lambda;
    fp_init(&xdiff);
    fp_init(&ydiff);
    fp_init(&lambda);

    // calculate lambda
    fp_sub((fp_int *)&op2->y, (fp_int *)&op1->y, &ydiff);
    fp_sub((fp_int *)&op2->x, (fp_int *)&op1->x, &xdiff);
    fp_invmod(&xdiff, (fp_int *)&curve->p, &xdiff);
    fp_mul(&ydiff, &xdiff, &lambda);
    fp_mod(&lambda, (fp_int *)&curve->p, &lambda);

    // calculate resulting x coord
    fp_mul(&lambda, &lambda, (fp_int *)&rop->x);
    fp_sub((fp_int *)&rop->x, (fp_int *)&op1->x, (fp_int *)&rop->x);
    fp_sub((fp_int *)&rop->x, (fp_int *)&op2->x, (fp_int *)&rop->x);
    fp_mod((fp_int *)&rop->x, (fp_int *)&curve->p, (fp_int *)&rop->x);

    // calculate resulting y coord
    fp_sub((fp_int *)&op1->x, (fp_int *)&rop->x, (fp_int *)&rop->y);
    fp_mul(&lambda, (fp_int *)&rop->y, (fp_int *)&rop->y);
    fp_sub((fp_int *)&rop->y, (fp_int *)&op1->y, (fp_int *)&rop->y);
    fp_mod((fp_int *)&rop->y, (fp_int *)&curve->p, (fp_int *)&rop->y);
}

STATIC void ec_point_mul(ecc_point_t *rop, const ecc_point_t *point, const fp_int scalar, const ecc_curve_t *curve)
{
    // handle the identity element
    if (fp_cmp_d((fp_int *)&point->x, 0) == FP_EQ && fp_cmp_d((fp_int *)&point->y, 0) == FP_EQ)
    {
        fp_set((fp_int *)&rop->x, 0);
        fp_set((fp_int *)&rop->y, 0);
        return;
    }

    bool scalar_is_negative = false;
    if (fp_cmp_d((fp_int *)&scalar, 2) == FP_EQ)
    {
        ec_point_double(rop, point, curve);
        return;
    }

    fp_int point_y_fp_int;
    if (fp_cmp_d((fp_int *)&scalar, 0) == FP_LT)
    {
        scalar_is_negative = true;
    }

    if (scalar_is_negative)
    {
        // copy point.y
        fp_copy((fp_int *)&point->y, &point_y_fp_int);
        // -point.y % curve.p
        fp_neg((fp_int *)&point->y, (fp_int *)&point->y);
        fp_mod((fp_int *)&point->y, (fp_int *)&curve->p, (fp_int *)&point->y);

        // -scalar
        fp_neg((fp_int *)&scalar, (fp_int *)&scalar);
    }

    ecc_point_t R0, R1, tmp;
    fp_init(&R1.x);
    fp_init(&R1.y);
    fp_init(&tmp.x);
    fp_init(&tmp.y);
    fp_copy((fp_int *)&point->x, (fp_int *)&R0.x);
    fp_copy((fp_int *)&point->y, (fp_int *)&R0.y);
    ec_point_double(&R1, point, curve);
    int dbits = fp_count_bits((fp_int *)&scalar), i;
    for (i = dbits - 2; i >= 0; i--)
    {
        if (fp_tstbit(scalar, i))
        {
            fp_copy((fp_int *)&R0.x, (fp_int *)&tmp.x);
            fp_copy((fp_int *)&R0.y, (fp_int *)&tmp.y);
            ec_point_add(&R0, &R1, &tmp, curve);
            fp_copy((fp_int *)&R1.x, (fp_int *)&tmp.x);
            fp_copy((fp_int *)&R1.y, (fp_int *)&tmp.y);
            ec_point_double(&R1, &tmp, curve);
        }
        else
        {
            fp_copy((fp_int *)&R1.x, (fp_int *)&tmp.x);
            fp_copy((fp_int *)&R1.y, (fp_int *)&tmp.y);
            ec_point_add(&R1, &R0, &tmp, curve);
            fp_copy((fp_int *)&R0.x, (fp_int *)&tmp.x);
            fp_copy((fp_int *)&R0.y, (fp_int *)&tmp.y);
            ec_point_double(&R0, &tmp, curve);
        }
    }

    fp_copy((fp_int *)&R0.x, (fp_int *)&rop->x);
    fp_copy((fp_int *)&R0.y, (fp_int *)&rop->y);

    if (scalar_is_negative)
    {
        // restore point.y
        fp_copy(&point_y_fp_int, (fp_int *)&point->y);
        // restore scalar
        fp_neg((fp_int *)&scalar, (fp_int *)&scalar);
    }
}

STATIC void ec_point_shamirs_trick(ecc_point_t *rop, const ecc_point_t *point1, const fp_int scalar1, const ecc_point_t *point2, const fp_int scalar2, const ecc_curve_t *curve)
{
    ecc_point_t sum, tmp;
    fp_init(&sum.x);
    fp_init(&sum.y);
    fp_init(&tmp.x);
    fp_init(&tmp.y);
    ec_point_add(&sum, point1, point2, curve);

    int scalar1Bits = fp_count_bits((fp_int *)&scalar1);
    int scalar2Bits = fp_count_bits((fp_int *)&scalar2);
    int l = (scalar1Bits > scalar2Bits ? scalar1Bits : scalar2Bits) - 1;

    if (fp_tstbit(scalar1, l) && fp_tstbit(scalar2, l))
    {
        fp_copy((fp_int *)&sum.x, (fp_int *)&rop->x);
        fp_copy((fp_int *)&sum.y, (fp_int *)&rop->y);
    }
    else if (fp_tstbit(scalar1, l))
    {
        fp_copy((fp_int *)&point1->x, (fp_int *)&rop->x);
        fp_copy((fp_int *)&point1->y, (fp_int *)&rop->y);
    }
    else if (fp_tstbit(scalar2, l))
    {
        fp_copy((fp_int *)&point2->x, (fp_int *)&rop->x);
        fp_copy((fp_int *)&point2->y, (fp_int *)&rop->y);
    }

    for (l = l - 1; l >= 0; l--)
    {
        fp_copy((fp_int *)&rop->x, (fp_int *)&tmp.x);
        fp_copy((fp_int *)&rop->y, (fp_int *)&tmp.y);
        ec_point_double(rop, &tmp, curve);

        fp_copy((fp_int *)&rop->x, (fp_int *)&tmp.x);
        fp_copy((fp_int *)&rop->y, (fp_int *)&tmp.y);

        if (fp_tstbit(scalar1, l) && fp_tstbit(scalar2, l))
        {
            ec_point_add(rop, &tmp, &sum, curve);
        }
        else if (fp_tstbit(scalar1, l))
        {
            ec_point_add(rop, &tmp, point1, curve);
        }
        else if (fp_tstbit(scalar2, l))
        {
            ec_point_add(rop, &tmp, point2, curve);
        }
    }
}

STATIC void ecdsa_s(ecdsa_signature_t *sig, unsigned char *msg, size_t msg_len, fp_int d, fp_int k, const ecc_curve_t *curve)
{
    fp_int e, kinv;

    // R = k * G, r = R[x]
    ecc_point_t R;
    fp_init(&R.x);
    fp_init(&R.y);
    ec_point_mul(&R, &curve->g, k, curve);
    fp_init(&sig->r);
    fp_copy((fp_int *)&R.x, (fp_int *)&sig->r);
    fp_mod((fp_int *)&sig->r, (fp_int *)&curve->q, (fp_int *)&sig->r);

    // convert digest to integer (digest is computed as hex in ecdsa.py)
    fp_read_radix(&e, (const char *)msg, 16);

    int orderBits = fp_count_bits((fp_int *)&curve->q);
    int digestBits = msg_len * 4;

    if (digestBits > orderBits)
    {
        fp_int n;
        fp_init(&n);
        fp_2expt(&n, digestBits - orderBits);
        fp_div(&e, &n, &e, NULL);
    }

    // s = (k^-1 * (e + d * r)) mod n
    fp_init(&kinv);
    fp_invmod(&k, (fp_int *)&curve->q, &kinv);
    fp_init(&sig->s);
    fp_mul(&d, (fp_int *)&sig->r, (fp_int *)&sig->s);
    fp_add((fp_int *)&sig->s, &e, (fp_int *)&sig->s);
    fp_mul((fp_int *)&sig->s, &kinv, (fp_int *)&sig->s);
    fp_mod((fp_int *)&sig->s, (fp_int *)&curve->q, (fp_int *)&sig->s);
}

STATIC int ecdsa_v(ecdsa_signature_t *sig, unsigned char *msg, size_t msg_len, ecc_point_t *Q, const ecc_curve_t *curve)
{
    fp_int e, w, u1, u2;
    ecc_point_t tmp;
    fp_init(&w);
    fp_init(&u1);
    fp_init(&u2);
    fp_init(&tmp.x);
    fp_init(&tmp.y);

    // convert digest to integer (digest is computed as hex in ecdsa.py)
    fp_read_radix(&e, (const char *)msg, 16);

    int orderBits = fp_count_bits((fp_int *)&curve->q);
    int digestBits = msg_len * 4;

    if (digestBits > orderBits)
    {
        fp_int tmp_;
        fp_init(&tmp_);
        fp_2expt(&tmp_, digestBits - orderBits);
        fp_div(&e, &tmp_, &e, NULL);
    }

    fp_invmod((fp_int *)&sig->s, (fp_int *)&curve->q, &w);
    fp_mul(&e, &w, &u1);
    fp_mod(&u1, (fp_int *)&curve->q, &u1);
    fp_mul((fp_int *)&sig->r, &w, &u2);
    fp_mod(&u2, (fp_int *)&curve->q, &u2);

    ec_point_shamirs_trick(&tmp, &curve->g, u1, Q, u2, curve);
    fp_mod((fp_int *)&tmp.x, (fp_int *)&curve->q, (fp_int *)&tmp.x);

    int equal = (fp_cmp((fp_int *)&tmp.x, (fp_int *)&sig->r) == FP_EQ);
    return equal;
}

STATIC mp_obj_t point_equal(mp_obj_t point1, mp_obj_t point2)
{
    if (!MP_OBJ_IS_TYPE(point1, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 1, mp_obj_get_type_str(point1));
    }
    if (!MP_OBJ_IS_TYPE(point2, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 2, mp_obj_get_type_str(point2));
    }

    mp_point_t *p1 = MP_OBJ_TO_PTR(point1);
    mp_point_t *p2 = MP_OBJ_TO_PTR(point2);
    return mp_obj_new_bool(ec_point_equal(&p1->ecc_point, &p2->ecc_point));
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(point_equal_obj, point_equal);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_point_equal_obj, MP_ROM_PTR(&point_equal_obj));

STATIC mp_obj_t point_double(mp_obj_t point, mp_obj_t curve)
{
    if (!MP_OBJ_IS_TYPE(point, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 1, mp_obj_get_type_str(point));
    }
    if (!MP_OBJ_IS_TYPE(curve, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 2, mp_obj_get_type_str(curve));
    }

    mp_point_t *p = MP_OBJ_TO_PTR(point);
    mp_curve_t *c = MP_OBJ_TO_PTR(curve);

    mp_point_t *pr = new_point_init_copy(c);
    ec_point_double(&pr->ecc_point, &p->ecc_point, &c->ecc_curve);
    return (mp_obj_t)pr;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(point_double_obj, point_double);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_point_double_obj, MP_ROM_PTR(&point_double_obj));

STATIC mp_obj_t point_add(mp_obj_t point1, mp_obj_t point2, mp_obj_t curve)
{
    if (!MP_OBJ_IS_TYPE(point1, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 1, mp_obj_get_type_str(point1));
    }
    if (!MP_OBJ_IS_TYPE(point2, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 2, mp_obj_get_type_str(point2));
    }
    if (!MP_OBJ_IS_TYPE(curve, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 3, mp_obj_get_type_str(curve));
    }

    mp_point_t *p1 = MP_OBJ_TO_PTR(point1);
    mp_point_t *p2 = MP_OBJ_TO_PTR(point2);
    mp_curve_t *c = MP_OBJ_TO_PTR(curve);

    mp_point_t *pr = new_point_init_copy(c);
    ec_point_add(&pr->ecc_point, &p1->ecc_point, &p2->ecc_point, &c->ecc_curve);
    return (mp_obj_t)pr;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_3(point_add_obj, point_add);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_point_add_obj, MP_ROM_PTR(&point_add_obj));

STATIC mp_obj_t point_sub(mp_obj_t point1, mp_obj_t point2, mp_obj_t curve)
{
    if (!MP_OBJ_IS_TYPE(point1, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 1, mp_obj_get_type_str(point1));
    }
    if (!MP_OBJ_IS_TYPE(point2, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 2, mp_obj_get_type_str(point2));
    }
    if (!MP_OBJ_IS_TYPE(curve, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 3, mp_obj_get_type_str(curve));
    }

    mp_point_t *p1 = MP_OBJ_TO_PTR(point1);
    mp_point_t *p2 = MP_OBJ_TO_PTR(point2);
    mp_curve_t *c = MP_OBJ_TO_PTR(curve);

    // -point2.y % curve.p
    fp_int p2_y_fp_int;
    fp_copy((fp_int *)&p2->ecc_point.y, &p2_y_fp_int);
    fp_neg((fp_int *)&p2->ecc_point.y, (fp_int *)&p2->ecc_point.y);
    fp_mod((fp_int *)&p2->ecc_point.y, (fp_int *)&c->ecc_curve.p, (fp_int *)&p2->ecc_point.y);

    mp_point_t *pr = new_point_init_copy(c);
    ec_point_add(&pr->ecc_point, &p1->ecc_point, &p2->ecc_point, &c->ecc_curve);

    // restore point.y
    fp_copy(&p2_y_fp_int, (fp_int *)&p2->ecc_point.y);
    return (mp_obj_t)pr;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_3(point_sub_obj, point_sub);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_point_sub_obj, MP_ROM_PTR(&point_sub_obj));

STATIC mp_obj_t point_mul(mp_obj_t point, mp_obj_t scalar, mp_obj_t curve)
{
    if (!MP_OBJ_IS_TYPE(point, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 1, mp_obj_get_type_str(point));
    }
    if (!MP_OBJ_IS_INT(scalar))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT_AT_BUT, 2, mp_obj_get_type_str(scalar));
    }
    if (!MP_OBJ_IS_TYPE(curve, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 3, mp_obj_get_type_str(curve));
    }

    mp_point_t *p = MP_OBJ_TO_PTR(point);
    mp_curve_t *c = MP_OBJ_TO_PTR(curve);
    fp_int s_fp_int;
    mp_fp_for_int(scalar, &s_fp_int);

    mp_point_t *pr = new_point_init_copy(c);
    ec_point_mul(&pr->ecc_point, &p->ecc_point, s_fp_int, &c->ecc_curve);
    return (mp_obj_t)pr;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_3(point_mul_obj, point_mul);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_point_mul_obj, MP_ROM_PTR(&point_mul_obj));

STATIC mp_obj_t signature(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_r, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_s, MP_ARG_OBJ, {.u_obj = mp_const_none}}};

    struct
    {
        mp_arg_val_t r, s;
    } args;
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, (mp_arg_val_t *)&args);

    mp_ecdsa_signature_t *signature = m_new_obj(mp_ecdsa_signature_t);
    signature->base.type = &signature_type;
    for (size_t i = 0; i < n_args; i++)
    {
        if (!MP_OBJ_IS_INT(pos_args[i]))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT_AT_BUT, mp_obj_get_type_str(pos_args[i]), i);
        }
    }

    mp_fp_for_int(args.r.u_obj, &signature->ecdsa_signature.r);
    mp_fp_for_int(args.s.u_obj, &signature->ecdsa_signature.s);

    return (mp_obj_t)signature;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(signature_obj, 2, signature);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_signature_obj, MP_ROM_PTR(&signature_obj));

STATIC mp_obj_t ecdsa_sign(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    mp_obj_t msg = args[0];
    mp_obj_t d = args[1];
    mp_obj_t k = args[2];
    mp_obj_t curve = args[3];

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(msg, &bufinfo, MP_BUFFER_READ);
    if (!MP_OBJ_IS_INT(d))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT_AT_BUT, 2, mp_obj_get_type_str(d));
    }
    if (!MP_OBJ_IS_INT(k))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT_AT_BUT, 3, mp_obj_get_type_str(k));
    }
    if (!MP_OBJ_IS_TYPE(curve, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 4, mp_obj_get_type_str(curve));
    }

    mp_curve_t *c = MP_OBJ_TO_PTR(curve);
    fp_int d_fp_int;
    mp_fp_for_int(d, &d_fp_int);
    fp_int k_fp_int;
    mp_fp_for_int(k, &k_fp_int);

    mp_ecdsa_signature_t *sr = m_new_obj(mp_ecdsa_signature_t);
    sr->base.type = &signature_type;
    ecdsa_s(&sr->ecdsa_signature, bufinfo.buf, bufinfo.len, d_fp_int, k_fp_int, &c->ecc_curve);
    return (mp_obj_t)sr;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ecdsa_sign_obj, 4, 4, ecdsa_sign);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_ecdsa_sign_obj, MP_ROM_PTR(&ecdsa_sign_obj));

STATIC mp_obj_t ecdsa_verify(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    mp_obj_t signature = args[0];
    mp_obj_t msg = args[1];
    mp_obj_t Q = args[2];
    mp_obj_t curve = args[3];

    if (!MP_OBJ_IS_TYPE(signature, &signature_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_SIGNATURE_AT_BUT, 1, mp_obj_get_type_str(signature));
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(msg, &bufinfo, MP_BUFFER_READ);
    if (!MP_OBJ_IS_TYPE(Q, &point_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_POINT_AT_BUT, 3, mp_obj_get_type_str(Q));
    }
    if (!MP_OBJ_IS_TYPE(curve, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_AT_BUT, 4, mp_obj_get_type_str(curve));
    }

    mp_ecdsa_signature_t *s = MP_OBJ_TO_PTR(signature);
    mp_point_t *q = MP_OBJ_TO_PTR(Q);
    mp_curve_t *c = MP_OBJ_TO_PTR(curve);
    return mp_obj_new_bool(ecdsa_v(&s->ecdsa_signature, bufinfo.buf, bufinfo.len, &q->ecc_point, &c->ecc_curve));
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ecdsa_verify_obj, 4, 4, ecdsa_verify);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_ecdsa_verify_obj, MP_ROM_PTR(&ecdsa_verify_obj));

STATIC void point_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_point_t *self = MP_OBJ_TO_PTR(self_in);
    vstr_t vstr_oid;
    vstr_hexlify(&vstr_oid, (const byte *)vstr_str(&self->ecc_curve.oid), vstr_len(&self->ecc_curve.oid));
    vstr_t *ecc_point_x = vstr_new_from_fp((fp_int *)&self->ecc_point.x);
    vstr_t *ecc_point_y = vstr_new_from_fp((fp_int *)&self->ecc_point.y);
    vstr_t *ecc_curve_p = vstr_new_from_fp((fp_int *)&self->ecc_curve.p);
    vstr_t *ecc_curve_a = vstr_new_from_fp((fp_int *)&self->ecc_curve.a);
    vstr_t *ecc_curve_b = vstr_new_from_fp((fp_int *)&self->ecc_curve.b);
    vstr_t *ecc_curve_q = vstr_new_from_fp((fp_int *)&self->ecc_curve.q);
    vstr_t *ecc_curve_g_x = vstr_new_from_fp((fp_int *)&self->ecc_curve.g.x);
    vstr_t *ecc_curve_g_y = vstr_new_from_fp((fp_int *)&self->ecc_curve.g.y);
    mp_printf(print, "<Point x=%s y=%s curve=<Curve name=%s oid=%s p=%s a=%s b=%s q=%s gx=%s gy=%s>>", vstr_str(ecc_point_x), vstr_str(ecc_point_y), vstr_str(&self->ecc_curve.name), vstr_str(&vstr_oid), vstr_str(ecc_curve_p), vstr_str(ecc_curve_a), vstr_str(ecc_curve_b), vstr_str(ecc_curve_q), vstr_str(ecc_curve_g_x), vstr_str(ecc_curve_g_y));
    vstr_free(ecc_point_x);
    vstr_free(ecc_point_y);
    vstr_free(ecc_curve_p);
    vstr_free(ecc_curve_a);
    vstr_free(ecc_curve_b);
    vstr_free(ecc_curve_q);
    vstr_free(ecc_curve_g_x);
    vstr_free(ecc_curve_g_y);
}

STATIC void point_attr(mp_obj_t obj, qstr attr, mp_obj_t *dest)
{
    mp_point_t *self = MP_OBJ_TO_PTR(obj);
    if (dest[0] == MP_OBJ_NULL)
    {
        const mp_obj_type_t *type = mp_obj_get_type(obj);
        mp_map_t *locals_map = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
        mp_map_elem_t *elem = mp_map_lookup(locals_map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
        if (elem != NULL)
        {
            if (attr == MP_QSTR_x)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_point.x);
                return;
            }
            else if (attr == MP_QSTR_y)
            {
                dest[0] = mp_obj_new_int_from_fp(&self->ecc_point.y);
                return;
            }
            else if (attr == MP_QSTR_curve)
            {
                dest[0] = new_curve_init_copy(self);
                return;
            }
            mp_convert_member_lookup(obj, type, elem->value, dest);
        }
    }
    else
    {
        if ((attr == MP_QSTR_x || attr == MP_QSTR_y) && !MP_OBJ_IS_INT(dest[1]))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT, mp_obj_get_type_str(dest[1]));
        }
        else if (attr == MP_QSTR_curve && !MP_OBJ_IS_TYPE(dest[1], &curve_type))
        {
            mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_BUT, mp_obj_get_type_str(dest[1]));
        }

        if (attr == MP_QSTR_x)
        {
            mp_fp_for_int(dest[1], &self->ecc_point.x);
        }
        else if (attr == MP_QSTR_y)
        {
            mp_fp_for_int(dest[1], &self->ecc_point.y);
        }
        else if (attr == MP_QSTR_curve)
        {
            mp_curve_t *other = MP_OBJ_TO_PTR(dest[1]);
            vstr_init(&self->ecc_curve.name, vstr_len(&other->ecc_curve.oid));
            vstr_add_strn(&self->ecc_curve.name, vstr_str(&other->ecc_curve.name), vstr_len(&other->ecc_curve.name));
            vstr_init(&self->ecc_curve.oid, vstr_len(&other->ecc_curve.oid));
            vstr_add_strn(&self->ecc_curve.oid, vstr_str(&other->ecc_curve.oid), vstr_len(&other->ecc_curve.oid));
            fp_copy(&other->ecc_curve.p, &self->ecc_curve.p);
            fp_copy(&other->ecc_curve.a, &self->ecc_curve.a);
            fp_copy(&other->ecc_curve.b, &self->ecc_curve.b);
            fp_copy(&other->ecc_curve.q, &self->ecc_curve.q);
            fp_copy(&other->ecc_curve.g.x, &self->ecc_curve.g.x);
            fp_copy(&other->ecc_curve.g.y, &self->ecc_curve.g.y);
        }
        else
        {
            return;
        }
        dest[0] = MP_OBJ_NULL; // indicate success
    }
}

STATIC mp_obj_t point_binary_op(mp_binary_op_t op, mp_obj_t lhs, mp_obj_t rhs)
{
    switch (op)
    {
    case MP_BINARY_OP_ADD:
    {
        if (!MP_OBJ_IS_TYPE(lhs, &point_type) && !MP_OBJ_IS_TYPE(rhs, &point_type))
        {
            mp_raise_TypeError(ERROR_EXPECTED_POINTS);
        }
        mp_point_t *l = MP_OBJ_TO_PTR(lhs);
        mp_point_t *r = MP_OBJ_TO_PTR(rhs);
        if (!ec_curve_equal(&l->ecc_curve, &r->ecc_curve))
        {
            mp_raise_ValueError(ERROR_CURVE_OF_POINTS_NOT_EQUAL);
        }

        mp_curve_t *c = new_curve_init_copy(l);
        return point_add((mp_obj_t)l, (mp_obj_t)r, (mp_obj_t)c);
    }
    case MP_BINARY_OP_SUBTRACT:
    {
        if (!MP_OBJ_IS_TYPE(lhs, &point_type) && !MP_OBJ_IS_TYPE(rhs, &point_type))
        {
            mp_raise_TypeError(ERROR_EXPECTED_POINTS);
        }
        mp_point_t *l = MP_OBJ_TO_PTR(lhs);
        mp_point_t *r = MP_OBJ_TO_PTR(rhs);
        if (!ec_curve_equal(&l->ecc_curve, &r->ecc_curve))
        {
            mp_raise_ValueError(ERROR_CURVE_OF_POINTS_NOT_EQUAL);
        }

        mp_curve_t *c = new_curve_init_copy(l);
        return point_sub((mp_obj_t)l, (mp_obj_t)r, (mp_obj_t)c);
    }
    case MP_BINARY_OP_MULTIPLY:
#if defined(MICROPY_PY_ALL_SPECIAL_METHODS) && defined(MICROPY_PY_REVERSE_SPECIAL_METHODS)
    case MP_BINARY_OP_REVERSE_MULTIPLY:
#endif
    {
        if (!MP_OBJ_IS_TYPE(lhs, &point_type))
        {
            mp_raise_TypeError(ERROR_LEFT_EXPECTED_POINT);
        }
        if (!MP_OBJ_IS_INT(rhs))
        {
            mp_raise_TypeError(ERROR_RIGHT_EXPECTED_INT);
        }
        mp_point_t *l = MP_OBJ_TO_PTR(lhs);
        mp_curve_t *c = new_curve_init_copy(l);
        return point_mul((mp_obj_t)l, rhs, (mp_obj_t)c);
    }
    case MP_BINARY_OP_EQUAL:
    {
        if (!MP_OBJ_IS_TYPE(lhs, &point_type) && !MP_OBJ_IS_TYPE(rhs, &point_type))
        {
            mp_raise_TypeError(ERROR_EXPECTED_POINTS);
        }
        mp_point_t *l = MP_OBJ_TO_PTR(lhs);
        mp_point_t *r = MP_OBJ_TO_PTR(rhs);
        if (!ec_curve_equal(&l->ecc_curve, &r->ecc_curve))
        {
            mp_raise_ValueError(ERROR_CURVE_OF_POINTS_NOT_EQUAL);
        }
        return point_equal((mp_obj_t)l, (mp_obj_t)r);
    }
    default:
        return MP_OBJ_NULL; // op not supported
    }
}

STATIC mp_obj_t point_unary_op(mp_unary_op_t op, mp_obj_t self_in)
{
    mp_point_t *point = MP_OBJ_TO_PTR(self_in);
    switch (op)
    {
    case MP_UNARY_OP_NEGATIVE:
    {
        // copy point.y
        fp_int point_y_fp_int;
        fp_copy((fp_int *)&point->ecc_point.y, &point_y_fp_int);

        // -point.y % curve.p
        fp_neg((fp_int *)&point->ecc_point.y, (fp_int *)&point->ecc_point.y);
        fp_mod((fp_int *)&point->ecc_point.y, (fp_int *)&point->ecc_curve.p, (fp_int *)&point->ecc_point.y);

        mp_point_t *pr = new_point_init_copy((mp_curve_t *)point);

        // restore point.y
        fp_copy(&point_y_fp_int, (fp_int *)&point->ecc_point.y);

        return (mp_obj_t)pr;
    }
    default:
        return MP_OBJ_NULL; // op not supported
    }
}

STATIC const mp_rom_map_elem_t point_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_x), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_y), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_curve), MP_ROM_PTR(mp_const_none)},
};

STATIC MP_DEFINE_CONST_DICT(point_locals_dict, point_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    point_type,
    MP_QSTR_Point,
    MP_TYPE_FLAG_NONE,
    print, point_print,
    binary_op, point_binary_op,
    unary_op, point_unary_op,
    attr, point_attr,
    locals_dict, &point_locals_dict);

STATIC mp_obj_t point(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_x, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_y, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_curve, MP_ARG_OBJ, {.u_obj = mp_const_none}},
    };

    struct
    {
        mp_arg_val_t x, y, curve;
    } args;
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, (mp_arg_val_t *)&args);

    mp_point_t *point = m_new_obj(mp_point_t);
    point->base.type = &point_type;

    if (!MP_OBJ_IS_INT(args.x.u_obj))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT_AT_BUT, mp_obj_get_type_str(args.x.u_obj), 1);
    }
    else
    {
        mp_fp_for_int(args.x.u_obj, &point->ecc_point.x);
    }

    if (!MP_OBJ_IS_INT(args.y.u_obj))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_INT_AT_BUT, mp_obj_get_type_str(args.y.u_obj), 2);
    }
    else
    {
        mp_fp_for_int(args.y.u_obj, &point->ecc_point.y);
    }

    if (!MP_OBJ_IS_TYPE(args.curve.u_obj, &curve_type))
    {
        mp_raise_msg_varg(&mp_type_TypeError, ERROR_EXPECTED_CURVE_BUT, mp_obj_get_type_str(args.curve.u_obj));
    }
    else
    {
        mp_curve_t *curve = MP_OBJ_TO_PTR(args.curve.u_obj);
        vstr_init(&point->ecc_curve.name, vstr_len(&curve->ecc_curve.oid));
        vstr_add_strn(&point->ecc_curve.name, vstr_str(&curve->ecc_curve.name), vstr_len(&curve->ecc_curve.name));
        vstr_init(&point->ecc_curve.oid, vstr_len(&curve->ecc_curve.oid));
        vstr_add_strn(&point->ecc_curve.oid, vstr_str(&curve->ecc_curve.oid), vstr_len(&curve->ecc_curve.oid));
        fp_copy(&curve->ecc_curve.p, &point->ecc_curve.p);
        fp_copy(&curve->ecc_curve.a, &point->ecc_curve.a);
        fp_copy(&curve->ecc_curve.b, &point->ecc_curve.b);
        fp_copy(&curve->ecc_curve.q, &point->ecc_curve.q);
        fp_copy(&curve->ecc_curve.g.x, &point->ecc_curve.g.x);
        fp_copy(&curve->ecc_curve.g.y, &point->ecc_curve.g.y);
    }
    return (mp_obj_t)point;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_KW(point_obj, 3, point);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(static_point_obj, MP_ROM_PTR(&point_obj));

STATIC void ecc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_printf(print, mp_obj_get_type_str(self_in));
}

STATIC const mp_rom_map_elem_t ecc_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_Point), MP_ROM_PTR(&static_point_obj)},
    {MP_ROM_QSTR(MP_QSTR_point_equal), MP_ROM_PTR(&static_point_equal_obj)},
    {MP_ROM_QSTR(MP_QSTR_point_double), MP_ROM_PTR(&static_point_double_obj)},
    {MP_ROM_QSTR(MP_QSTR_point_add), MP_ROM_PTR(&static_point_add_obj)},
    {MP_ROM_QSTR(MP_QSTR_point_sub), MP_ROM_PTR(&static_point_sub_obj)},
    {MP_ROM_QSTR(MP_QSTR_point_mul), MP_ROM_PTR(&static_point_mul_obj)},
    {MP_ROM_QSTR(MP_QSTR_Curve), MP_ROM_PTR(&static_curve_obj)},
    {MP_ROM_QSTR(MP_QSTR_curve_equal), MP_ROM_PTR(&static_curve_equal_obj)},
    {MP_ROM_QSTR(MP_QSTR_point_in_curve), MP_ROM_PTR(&static_point_in_curve_obj)},
    {MP_ROM_QSTR(MP_QSTR_Signature), MP_ROM_PTR(&static_signature_obj)},
    {MP_ROM_QSTR(MP_QSTR_ecdsa_sign), MP_ROM_PTR(&static_ecdsa_sign_obj)},
    {MP_ROM_QSTR(MP_QSTR_ecdsa_verify), MP_ROM_PTR(&static_ecdsa_verify_obj)},
};

STATIC MP_DEFINE_CONST_DICT(ecc_locals_dict, ecc_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ecc_type,
    MP_QSTR_ECC,
    MP_TYPE_FLAG_NONE,
    print, ecc_print,
    locals_dict, &ecc_locals_dict);

STATIC const mp_rom_map_elem_t mp_module_ucrypto_globals_table[] = {
    {MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR__crypto)},
    {MP_ROM_QSTR(MP_QSTR_ECC), MP_ROM_PTR(&ecc_type)},
    {MP_ROM_QSTR(MP_QSTR_NUMBER), MP_ROM_PTR(&number_type)},
};

STATIC MP_DEFINE_CONST_DICT(mp_module_ucrypto_globals, mp_module_ucrypto_globals_table);

const mp_obj_module_t mp_module_ucrypto = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&mp_module_ucrypto_globals,
};

// Register the module to make it available in Python
MP_REGISTER_MODULE(MP_QSTR__crypto, mp_module_ucrypto);

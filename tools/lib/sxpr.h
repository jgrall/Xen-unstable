/*
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or  (at your option) any later version. This library is 
 * distributed in the  hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */
#ifndef _XEN_LIB_SXPR_H_
#define _XEN_LIB_SXPR_H_

#include <stdint.h>

#include "hash_table.h"
#include "iostream.h"
#include "allocate.h"

/** @file
 * Definitions for rules and sxprs.
 */

#ifndef NULL
#define NULL 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

/** Sxpr type. */
typedef int16_t TypeCode;

/** A typed sxpr handle.*/
typedef struct Sxpr {
    /** Sxpr type. */
    TypeCode type;
    union {
	/** Sxpr value. */
        unsigned long ul;
	/** Pointer. */
        void *ptr;
    } v;
} Sxpr;

/** Sxpr type to indicate out of memory. */
#define T_NOMEM      ((TypeCode)-1)
/** The 'unspecified' sxpr. */
#define T_NONE       ((TypeCode)0)
/** The empty list. */
#define T_NULL       ((TypeCode)1)
/** Unsigned integer. */
#define T_UINT       ((TypeCode)2)
/** A string. */
#define T_STRING     ((TypeCode)3)
/** An atom. */
#define T_ATOM       ((TypeCode)4)
/** A boolean. */
#define T_BOOL       ((TypeCode)5)

/** A cons (pair or list). */
#define T_CONS       ((TypeCode)10)

/** An error. */
#define T_ERR        ((TypeCode)40)

/** An atom. */
typedef struct ObjAtom {
    Sxpr name;
    Hashcode hashcode;
    int interned;
} ObjAtom;

/** A cons (pair). */
typedef struct ObjCons {
    Sxpr car;
    Sxpr cdr;
} ObjCons;

/** A vector. */
typedef struct ObjVector {
    int n;
    Sxpr data[0];
} ObjVector;

/** Flags for sxpr printing. */
enum PrintFlags {
    PRINT_RAW           = 0x001,
    PRINT_TYPE          = 0x002,
    PRINT_PRETTY        = 0x004,
    PRINT_NUM           = 0x008,
};

/** An integer sxpr.
 *
 * @param ty type
 * @param val integer value
 */
#define OBJI(ty, val) (Sxpr){ type: (ty), v: { ul: (val) }}

/** A pointer sxpr.
 * If the pointer is non-null, returns an sxpr containing it.
 * If the pointer is null, returns ONOMEM.
 *
 * @param ty type
 * @param val pointer
 */
#define OBJP(ty, val) ((val) ? (Sxpr){ type: (ty), v: { ptr: (val) }} : ONOMEM)

/** Make an integer sxpr containing a pointer.
 *
 * @param val pointer
 */
#define PTR(val) OBJP(T_UINT, (void*)(val))

/** Make an integer sxpr.
 * @param x value
 */
#define OINT(x)       OBJI(T_UINT,  x)

/** Make an error sxpr.
 *
 * @param x value
 */
#define OERR(x)       OBJI(T_ERR,   x)

/** Out of memory constant. */
#define ONOMEM        OBJI(T_NOMEM, 0)

/** The `unspecified' constant. */
#define ONONE         OBJI(T_NONE,  0)

/** Empty list constant. */
#define ONULL         OBJI(T_NULL,  0)

/** False constant. */
#define OFALSE        OBJI(T_BOOL,  0)

/** True constant. */
#define OTRUE         OBJI(T_BOOL,  1)

/* Recognizers for the various sxpr types.  */
#define ATOMP(obj)        has_type(obj, T_ATOM)
#define BOOLP(obj)        has_type(obj, T_BOOL)
#define CONSP(obj)        has_type(obj, T_CONS)
#define ERRP(obj)         has_type(obj, T_ERR)
#define INTP(obj)         has_type(obj, T_UINT)
#define NOMEMP(obj)       has_type(obj, T_NOMEM)
#define NONEP(obj)        has_type(obj, T_NONE)
#define NULLP(obj)        has_type(obj, T_NULL)
#define STRINGP(obj)      has_type(obj, T_STRING)

#define TRUEP(obj)    get_ul(obj)

/** Convert an sxpr to an unsigned integer. */
#define OBJ_UINT(x)   get_ul(x)
/** Convert an sxpr to an integer. */
#define OBJ_INT(x)    (int)get_ul(x)

/* Conversions of sxprs to their values.
 * No checking is done.
 */
#define OBJ_STRING(x)  ((char*)get_ptr(x))
#define OBJ_CONS(x)    ((ObjCons*)get_ptr(x))
#define OBJ_ATOM(x)    ((ObjAtom*)get_ptr(x))
#define OBJ_SET(x)     ((ObjSet*)get_ptr(x))
#define CAR(x)         (OBJ_CONS(x)->car)
#define CDR(x)         (OBJ_CONS(x)->cdr)

#define CAAR(x)        (CAR(CAR(x)))
#define CADR(x)        (CAR(CDR(x)))
#define CDAR(x)        (CDR(CAR(x)))
#define CDDR(x)        (CDR(CDR(x)))

/** Get the integer value from an sxpr.
 *
 * @param obj sxpr
 * @return value
 */
static inline unsigned long get_ul(Sxpr obj){
    return obj.v.ul;
}

/** Get the pointer value from an sxpr.
 *
 * @param obj sxpr
 * @return value
 */
static inline void * get_ptr(Sxpr obj){
    return obj.v.ptr;
}

/** Create an sxpr containing a pointer.
 *
 * @param type typecode
 * @param val pointer
 * @return sxpr
 */
static inline Sxpr obj_ptr(TypeCode type, void *val){
    return (Sxpr){ type: type, v: { ptr: val } };
}

/** Create an sxpr containing an integer.
 *
 * @param type typecode
 * @param val integer
 * @return sxpr
 */
static inline Sxpr obj_ul(TypeCode type, unsigned long val){
    return (Sxpr){ type: type, v: { ul: val } };
}

/** Get the type of an sxpr.
 *
 * @param obj sxpr
 * @return type
 */
static inline TypeCode get_type(Sxpr obj){
    return obj.type;
}

/** Check the type of an sxpr.
 *
 * @param obj sxpr
 * @param type to check
 * @return 1 if has the type, 0 otherwise
 */
static inline int has_type(Sxpr obj, TypeCode type){
    return get_type(obj) == type;
}

/** Compare sxprs for literal equality of type and value.
 *
 * @param x sxpr to compare
 * @param y sxpr to compare
 * @return 1 if equal, 0 otherwise
 */
static inline int eq(Sxpr x, Sxpr y){
    return ((get_type(x) == get_type(y)) && (get_ul(x) == get_ul(y)));
}

/** Checked version of CAR
 *
 * @param x sxpr
 * @return CAR if a cons, x otherwise
 */
static inline Sxpr car(Sxpr x){
    return (CONSP(x) ? CAR(x) : x);
}

/** Checked version of CDR.
 *
 * @param x sxpr
 * @return CDR if a cons, null otherwise
 */
static inline Sxpr cdr(Sxpr x){
    return (CONSP(x) ? CDR(x) : ONULL);
}

/** Allocate some memory and return an sxpr containing it.
 * Returns ONOMEM if allocation failed.
 *
 * @param n number of bytes to allocate
 * @param ty typecode
 * @return sxpr
 */
static inline Sxpr halloc(size_t n,  TypeCode ty){
    return OBJP(ty, allocate(n));
}

/** Allocate an sxpr containing a pointer to the given type.
 *
 * @param ty type (uses sizeof to determine how many bytes to allocate)
 * @param code typecode
 * @return sxpr, ONOMEM if allocation failed
 */
#define HALLOC(ty, code) halloc(sizeof(ty), code)

typedef int ObjPrintFn(IOStream *io, Sxpr obj, unsigned flags);
typedef int ObjEqualFn(Sxpr obj, Sxpr other);
typedef void ObjFreeFn(Sxpr obj);

/** An sxpr type definition. */
typedef struct SxprType {
    TypeCode type;
    char *name;
    int pointer;
    ObjPrintFn *print;
    ObjEqualFn *equal;
    ObjFreeFn *free;
} SxprType;


extern SxprType *get_sxpr_type(int ty);

/** Free the pointer in an sxpr.
 *
 * @param x sxpr containing a pointer
 */
static inline void hfree(Sxpr x){
    deallocate(get_ptr(x));
}

extern int objprint(IOStream *io, Sxpr x, unsigned flags);
extern int objequal(Sxpr x, Sxpr y);
extern void objfree(Sxpr x);

extern void cons_free_cells(Sxpr obj);
extern Sxpr intern(char *s);

extern Sxpr assoc(Sxpr k, Sxpr l);
extern Sxpr assocq(Sxpr k, Sxpr l);
extern Sxpr acons(Sxpr k, Sxpr v, Sxpr l);
extern Sxpr nrev(Sxpr l);
extern Sxpr cons_member(Sxpr l, Sxpr x);
extern Sxpr cons_member_if(Sxpr l, ObjEqualFn *test_fn, Sxpr v);
extern int cons_subset(Sxpr s, Sxpr t);
extern int cons_set_equal(Sxpr s, Sxpr t);

#ifdef USE_GC
extern Sxpr cons_remove(Sxpr l, Sxpr x);
extern Sxpr cons_remove_if(Sxpr l, ObjEqualFn *test_fn, Sxpr v);
#endif

extern Sxpr atom_new(char *name);
extern char * atom_name(Sxpr obj);

extern Sxpr string_new(char *s);
extern char * string_string(Sxpr obj);
extern int string_length(Sxpr obj);

extern Sxpr cons_new(Sxpr car, Sxpr cdr);
extern int cons_push(Sxpr *list, Sxpr elt);
extern int cons_length(Sxpr obj);

Sxpr sxpr_name(Sxpr obj);
int sxpr_is(Sxpr obj, char *s);
int sxpr_elementp(Sxpr obj, Sxpr name);
Sxpr sxpr_attributes(Sxpr obj);
Sxpr sxpr_attribute(Sxpr obj, Sxpr key, Sxpr def);
Sxpr sxpr_children(Sxpr obj);
Sxpr sxpr_child(Sxpr obj, Sxpr name, Sxpr def);
Sxpr sxpr_child0(Sxpr obj, Sxpr def);
Sxpr sxpr_child_value(Sxpr obj, Sxpr name, Sxpr def);

/** Create a new atom.
 *
 * @param s atom name
 * @return new atom
 */
static inline Sxpr mkatom(char *s){
    return atom_new(s);
}

/** Create a new string sxpr.
 *
 * @param s string bytes (copied)
 * @return new string
 */
static inline Sxpr mkstring(char *s){
    return string_new(s);
}

/** Create an integer sxpr.
 *
 * @param i value
 * @return sxpr
 */
static inline Sxpr mkint(int i){
    return OBJI(T_UINT, i);
}

/** Create a boolean sxpr.
 *
 * @param b value
 * @return sxpr
 */
static inline Sxpr mkbool(int b){
    return OBJI(T_BOOL, (b ? 1 : 0));
}

/* Constants used in parsing and printing. */
#define k_list_open    "("
#define c_list_open    '('
#define k_list_close   ")"
#define c_list_close   ')'
#define k_true         "true"
#define k_false        "false"

#define c_var          '$'
#define c_escape       '\\'
#define c_single_quote '\''
#define c_double_quote '"'
#define c_string_open  c_double_quote
#define c_string_close c_double_quote
#define c_data_open    '['
#define c_data_close   ']'
#define c_binary       '*'
#define c_eval         '!'
#define c_concat_open  '{'
#define c_concat_close '}'

#endif /* ! _XEN_LIB_SXPR_H_ */

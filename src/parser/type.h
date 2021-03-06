#ifndef TYPE_H
#define TYPE_H

#include <lacc/typetree.h>

#include <stdlib.h>
#include <string.h>

/* Add member to struct, union or function type. Update size and alignment of
 * fields. For functions taking variable number of arguments, the last member
 * should be passed as "...".
 */
void type_add_member(
    struct typetree *type,
    const char *member_name,
    const struct typetree *member_type);

/* Find type member of the given name, meaning struct or union field, or
 * function parameter. Returns NULL in the case no member is found.
 */
const struct member *find_type_member(
    const struct typetree *type,
    const char *name);

/* Allocate and initialize a new type. Take additional parameters for
 * initializing integer, pointer and array types, otherwise all-zero (default)
 * values.
 *
 *      type_init(T_SIGNED, [size])
 *      type_init(T_POINTER, [next])
 *      type_init(T_ARRAY, [next], [count])
 */
struct typetree *type_init(enum type tt, ...);

/* Create a tag type pointing to the provided object. Input type must be of
 * struct or union type.
 *
 * Usage of this is to avoid circular typetree graphs, and to let tagged types
 * be cv-qualified without mutating the original definition.
 */
struct typetree *type_tagged_copy(
    const struct typetree *type,
    const char *name);

int is_compatible(const struct typetree *l, const struct typetree *r);

/* Get the type the given POINTER is pointing to. Handles tag indirections for
 * pointers to typedef'ed object types.
 */
const struct typetree *type_deref(const struct typetree *type);

/* Find a common real type between operands used in an expression, giving the
 * type of the result.
 */
const struct typetree *usual_arithmetic_conversion(
    const struct typetree *t1,
    const struct typetree *t2);

/* Promote the given integer type to int or unsigned int, or do nothing if the
 * precision is already as wide. For example, unsigned short will be converted
 * to int.
 */
const struct typetree *promote_integer(const struct typetree *type);

/* Print type to buffer, returning how many characters were written.
 */
int snprinttype(const struct typetree *type, char *str, size_t size);

#endif

#if _XOPEN_SOURCE < 500
#  undef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 500 /* snprintf */
#endif
#include "type.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct typetree
    basic_type__void = { T_VOID },
    basic_type__const_void = { T_VOID, 0, Q_CONST },
    basic_type__char = { T_SIGNED, 1 },
    basic_type__short = { T_SIGNED, 2 },
    basic_type__int = { T_SIGNED, 4 },
    basic_type__long = { T_SIGNED, 8 },
    basic_type__unsigned_char = { T_UNSIGNED, 1 },
    basic_type__unsigned_short = { T_UNSIGNED, 2 },
    basic_type__unsigned_int = { T_UNSIGNED, 4 },
    basic_type__unsigned_long = { T_UNSIGNED, 8 },
    basic_type__float = { T_REAL, 4 },
    basic_type__double = { T_REAL, 8 };

/* Store member list separate from type to make memory ownership easier, types
 * do not own their member list.
 */
struct member_list {
    int length;
    int cap;
    int func_vararg;
    struct member *member;
};

static struct typetree **type_registry;
static size_t length;
static size_t cap;

static struct member_list **mem_list_registry;
static size_t mem_length, mem_cap;

static void cleanup(void)
{
    size_t i = 0;
    struct typetree *t;

    for ( ; i < length; ++i) {
        t = type_registry[i];
        free(t);
    }

    if (type_registry) {
        free(type_registry);
        type_registry = NULL;
        length = cap = 0;
    }

    for (i = 0; i < mem_length; ++i) {
        free(mem_list_registry[i]->member);
        free(mem_list_registry[i]);
    }

    if (mem_list_registry) {
        free(mem_list_registry);
        mem_list_registry = NULL;
        mem_length = 0;
        mem_cap = 0;
    }
}

static struct typetree *calloc_type(void)
{
    if (!length && !mem_length)
        atexit(cleanup);

    if (length == cap) {
        cap = (!cap) ? 32 : cap * 2;
        type_registry = realloc(type_registry, cap * sizeof(*type_registry));
    }

    type_registry[length] = calloc(1, sizeof(**type_registry));
    return type_registry[length++];
}

static struct member_list *allocmembers(void)
{
    if (!length && !mem_length)
        atexit(cleanup);

    if (mem_length == mem_cap) {
        mem_cap = (!mem_cap) ? 32 : mem_cap * 2;
        mem_list_registry =
            realloc(mem_list_registry, mem_cap * sizeof(*mem_list_registry));
    }

    mem_list_registry[mem_length] = calloc(1, sizeof(**mem_list_registry));
    return mem_list_registry[mem_length++];
}

int type_alignment(const struct typetree *type)
{
    int i = 0, m = 0, d;
    assert(is_object(type));

    switch (type->type) {
    case T_ARRAY:
        return type_alignment(type->next);
    case T_STRUCT:
    case T_UNION:
        type = unwrapped(type);
        for (; i < nmembers(type); ++i) {
            d = type_alignment(get_member(type, i)->type);
            if (d > m) m = d;
        }
        assert(m);
        return m;
    default:
        return type->size;
    }
}

static int align_struct_members(struct member_list *list)
{
    int i,
        size = 0,
        alignment,
        max_alignment = 0;
    struct member *field;

    for (i = 0; i < list->length; ++i) {
        field = &list->member[i];
        alignment = type_alignment(field->type);
        if (alignment > max_alignment) {
            max_alignment = alignment;
        }

        /* Add padding until size matches alignment. */
        if (size % alignment) {
            size += alignment - (size % alignment);
        }

        assert(!(size % alignment));
        field->offset = size;
        size += size_of(field->type);
    }

    /* Total size must be a multiple of strongest alignment. */
    if (size % max_alignment) {
        size += max_alignment - (size % max_alignment);
    }

    return size;
}

int nmembers(const struct typetree *type)
{
    return (type->member_list) ? type->member_list->length : 0;
}

const struct member *get_member(const struct typetree *type, int n)
{
    if (!type->member_list || type->member_list->length <= n)
        return NULL;
    return type->member_list->member + n;
}

void type_add_member(
    struct typetree *type,
    const char *member_name,
    const struct typetree *member_type)
{
    struct member_list *list;

    assert(is_struct_or_union(type) || is_function(type));
    assert(!is_function(type) || !is_vararg(type));
    assert(!is_tagged(type));

    if (!type->member_list)
        type->member_list = allocmembers();

    list = (struct member_list *) type->member_list;

    /* Adding function parameters have special case for "..." meaning variable
     * argument list, and array types decaying to pointer. */
    if (is_function(type)) {
        if (member_name && !strcmp(member_name, "...")) {
            list->func_vararg = 1;
            return;
        }
        if (is_array(member_type))
            member_type = type_init(T_POINTER, member_type->next);
    }

    if (list->length == list->cap) {
        list->cap = 2 * list->cap + 2;
        list->member = realloc(list->member, list->cap * sizeof(*list->member));
    }

    assert(list->length < list->cap);
    list->member[list->length].name = member_name;
    list->member[list->length].type = member_type;
    list->member[list->length].offset = 0;
    list->length++;

    /* Align new struct members immediately. */
    if (is_struct(type)) {
        type->size = align_struct_members(list);
    }

    /* Size of union is the largest of the fields. */
    if (is_union(type)) {
        if (type->size < size_of(member_type)) {
            type->size = size_of(member_type);
        }
    }
}

int is_vararg(const struct typetree *type)
{
    assert(is_function(type));

    return (type->member_list) ? type->member_list->func_vararg : 0;
}

struct typetree *type_init(enum type tt, ...)
{
    struct typetree *type;
    va_list args;
    va_start(args, tt);

    type = calloc_type();
    type->type = tt;
    if (tt == T_POINTER || tt == T_ARRAY) {
        type->next = va_arg(args, const struct typetree *);
        type->size = 8;
        if (tt == T_ARRAY) {
            type->size = size_of(type->next) * va_arg(args, int);
        }
    } else if (tt == T_UNSIGNED || tt == T_SIGNED) {
        type->size = va_arg(args, int);
        assert(
            type->size == 8 || type->size == 4 ||
            type->size == 2 || type->size == 1);
    }

    va_end(args);
    return type;
}

const struct typetree *unwrapped(const struct typetree *type)
{
    return is_tagged(type) ? type->next : type;
}

struct typetree *type_tagged_copy(const struct typetree *type, const char *name)
{
    struct typetree *tag;

    assert(!is_tagged(type));
    assert(is_struct_or_union(type));

    tag = calloc_type();
    tag->type = type->type;
    tag->tag_name = name;
    tag->next = type;
    return tag;
}

/* Determine whether two types are the same. Disregarding qualifiers, and names
 * of function parameters.
 */
int type_equal(const struct typetree *a, const struct typetree *b)
{
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (is_tagged(a) && is_tagged(b))
        return a->next == b->next;

    a = unwrapped(a);
    b = unwrapped(b);

    if (a->type == b->type
        && a->size == b->size
        && nmembers(a) == nmembers(b)
        && is_unsigned(a) == is_unsigned(b)
        && type_equal(a->next, b->next))
    {
        int i;
        for (i = 0; i < nmembers(a); ++i) {
            if (!type_equal(get_member(a, i)->type, get_member(b, i)->type)) {
                return 0;
            }
            if (is_struct_or_union(a)
                && strcmp(get_member(a, i)->name, get_member(b, i)->name))
            {
                return 0;
            }
            assert(get_member(a, i)->offset == get_member(b, i)->offset);
        }
        return 1;   
    }

    return 0;
}

static const struct typetree *remove_qualifiers(const struct typetree *type)
{
    if (type->qualifier) {
        struct typetree *copy = calloc_type();
        assert(!nmembers(type));

        *copy = *type;
        copy->qualifier = 0;
        type = copy;
    }

    return type;
}

const struct typetree *promote_integer(const struct typetree *type)
{
    assert(is_integer(type));

    if (type->size < 4) {
        type = (is_unsigned(type)) ?
            &basic_type__unsigned_int : &basic_type__int;
    }

    return type;
}

const struct typetree *usual_arithmetic_conversion(
    const struct typetree *t1,
    const struct typetree *t2)
{
    assert(is_arithmetic(t1) && is_arithmetic(t2));

    /* Skip everything dealing with floating point types. */

    assert(is_integer(t1) && is_integer(t2));
    t1 = promote_integer(t1);
    t2 = promote_integer(t2);

    if (t1->size > t2->size)
        /* TODO: This can be done without extra copies. */
        return remove_qualifiers(t1);
    else if (t2->size > t1->size)
        return remove_qualifiers(t2);

    return is_unsigned(t1) ? remove_qualifiers(t1) : remove_qualifiers(t2);
}

/* 6.2.7 Compatible types. Simplified rules.
 */
int is_compatible(const struct typetree *l, const struct typetree *r)
{
    return type_equal(l, r);
}

int size_of(const struct typetree *type)
{
    return is_tagged(type) ? type->next->size : type->size;
}

const struct typetree *type_deref(const struct typetree *type)
{
    assert(is_pointer(type));
    return unwrapped(type->next);
}

const struct member *find_type_member(
    const struct typetree *type,
    const char *name)
{
    int i;
    const struct member *member;

    assert(is_struct_or_union(type));

    type = unwrapped(type);
    for (i = 0; i < nmembers(type); ++i) {
        member = get_member(type, i);
        if (!strcmp(name, member->name)) {
            return member;
        }
    }

    return NULL;
}

int snprinttype(const struct typetree *tree, char *s, size_t size)
{
    size_t w = 0;
    int i;

    if (!tree) {
        return w;
    }

    if (is_const(tree)) w += snprintf(s + w, size - w, "const ");
    if (is_volatile(tree)) w += snprintf(s + w, size - w, "volatile ");

    if (is_tagged(tree)) {
        w += snprintf(s + w, size - w, "%s %s",
            (is_union(tree)) ? "union" : "struct", tree->tag_name);
        return w;
    }

    switch (tree->type) {
    case T_UNSIGNED:
        w += snprintf(s + w, size - w, "unsigned ");
    case T_SIGNED:
        switch (tree->size) {
        case 1:
            w += snprintf(s + w, size - w, "char");
            break;
        case 2:
            w += snprintf(s + w, size - w, "short");
            break;
        case 4:
            w += snprintf(s + w, size - w, "int");
            break;
        default:
            w += snprintf(s + w, size - w, "long");
            break;
        }
        break;
    case T_REAL:
        switch (tree->size) {
        case 4:
            w += snprintf(s + w, size - w, "float");
            break;
        default:
            w += snprintf(s + w, size - w, "double");
            break;
        }
        break;
    case T_VOID:
        w += snprintf(s + w, size - w, "void");
        break;
    case T_POINTER:
        w += snprintf(s + w, size - w, "* ");
        w += snprinttype(tree->next, s + w, size - w);
        break;
    case T_FUNCTION:
        w += snprintf(s + w, size - w, "(");
        for (i = 0; i < nmembers(tree); ++i) {
            w += snprinttype(get_member(tree, i)->type, s + w, size - w);
            if (i < nmembers(tree) - 1) {
                w += snprintf(s + w, size - w, ", ");
            }
        }
        if (is_vararg(tree)) {
            w += snprintf(s + w, size - w, ", ...");
        }
        w += snprintf(s + w, size - w, ") -> ");
        w += snprinttype(tree->next, s + w, size - w);
        break;
    case T_ARRAY:
        if (tree->size > 0) {
            w += snprintf(s + w, size - w, "[%u] ",
                tree->size / size_of(tree->next));
        } else {
            w += snprintf(s + w, size - w, "[] ");
        }
        w += snprinttype(tree->next, s + w, size - w);
        break;
    case T_STRUCT:
    case T_UNION:
        w += snprintf(s + w, size - w, "{");
        for (i = 0; i < nmembers(tree); ++i) {
            const struct member *member = get_member(tree, i);
            w += snprintf(s + w, size - w, ".%s::", member->name);
            w += snprinttype(member->type, s + w, size - w);
            w += snprintf(s + w, size - w, " (+%d)", member->offset);
            if (i < nmembers(tree) - 1) {
                w += snprintf(s + w, size - w, ", ");
            }
        }
        w += snprintf(s + w, size - w, "}");
        break;
    }

    return w;
}

char *typetostr(const struct typetree *type)
{
    char *text = malloc(2048 * sizeof(char));
    snprinttype(type, text, 2047);
    return text;
}

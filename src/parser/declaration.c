#include "declaration.h"
#include "statement.h"
#include "eval.h"
#include "expression.h"
#include "symtab.h"
#include "type.h"
#include <lacc/token.h>
#include <lacc/cli.h>

#include <assert.h>

/* Parser consumes whole declaration statements, which can include multiple
 * definitions. For example 'int foo = 1, bar = 2;'. These are buffered and
 * returned one by one on calls to parse().
 */
static struct definition_list {
    struct definition *def;
    int cur;                    /* Index of definition to return next */
    int len;
} defs;

static struct definition fallback;

static struct block *initializer(struct block *block, struct var target);

static struct definition *push_back_definition(const struct symbol *sym)
{
    struct definition *def;
    assert(sym->symtype == SYM_DEFINITION);

    defs.def = realloc(defs.def, (defs.len + 1) * sizeof(*defs.def));
    def = &defs.def[defs.len++];

    memset(def, 0, sizeof(*def));
    def->symbol = sym;
    def->body = cfg_block_init();
    return def;
}

static void clear_definition(struct definition *def)
{
    int i;
    struct block *block;
    if (def->params.capacity) free(def->params.symbol);
    if (def->locals.capacity) free(def->locals.symbol);
    if (def->nodes.capacity) {
        for (i = 0; i < def->nodes.length; ++i) {
            block = def->nodes.block[i];
            if (block->n)
                free(block->code);
            free(block);
        }
        free(def->nodes.block);
    }
    memset(def, 0, sizeof(*def));
}

static int is_string(struct var val)
{
    return
        val.kind == IMMEDIATE && val.symbol &&
        val.symbol->symtype == SYM_STRING_VALUE;
}

static struct var var_zero(int size)
{
    struct var var = {0};

    var.kind = IMMEDIATE;
    var.type = BASIC_TYPE_SIGNED(size);
    var.imm.i = 0;
    return var;
}

static struct block_list block_list_add(
    struct block_list list,
    struct block *block)
{
    assert(block);
    if (list.capacity <= list.length) {
        list.capacity += 32;
        list.block = realloc(list.block, list.capacity * sizeof(*block));
    }

    list.block[list.length++] = block;
    return list;
}

static struct symbol_list sym_list_add(
    struct symbol_list list,
    struct symbol *sym)
{
    assert(sym);
    if (list.capacity <= list.length) {
        list.capacity += 32;
        list.symbol = realloc(list.symbol, list.capacity * sizeof(*sym));
    }

    list.symbol[list.length++] = sym;
    return list;
}

/* FOLLOW(parameter-list) = { ')' }, peek to return empty list; even though K&R
 * require at least specifier: (void)
 * Set parameter-type-list = parameter-list, including the , ...
 */
static struct typetree *parameter_list(const struct typetree *base)
{
    struct typetree *func = type_init(T_FUNCTION);
    func->next = base;

    while (peek().token != ')') {
        const char *name = NULL;
        struct typetree *type;

        type = declaration_specifiers(NULL);
        type = declarator(type, &name);
        if (is_void(type)) {
            if (nmembers(func)) {
                error("Incomplete type in parameter list.");
            }
            break;
        }

        type_add_member(func, name, type);
        if (peek().token != ',') {
            break;
        }

        consume(',');
        if (peek().token == ')') {
            error("Unexpected trailing comma in parameter list.");
            exit(1);
        } else if (peek().token == DOTS) {
            consume(DOTS);
            assert(!is_vararg(func));
            type_add_member(func, "...", NULL);
            assert(is_vararg(func));
            break;
        }
    }

    return func;
}

/* Parse array declarations of the form [s0][s1]..[sn], resulting in type
 * [s0] [s1] .. [sn] (base).
 *
 * Only the first dimension s0 can be unspecified, yielding an incomplete type.
 * Incomplete types are represented by having size of zero.
 */
static struct typetree *direct_declarator_array(struct typetree *base)
{
    if (peek().token == '[') {
        long length = 0;

        consume('[');
        if (peek().token != ']') {
            struct var expr = constant_expression();
            assert(expr.kind == IMMEDIATE);
            if (!is_integer(expr.type) || expr.imm.i < 1) {
                error("Array dimension must be a natural number.");
                exit(1);
            }
            length = expr.imm.i;
        }
        consume(']');

        base = direct_declarator_array(base);
        if (!size_of(base)) {
            error("Array has incomplete element type.");
            exit(1);
        }

        base = type_init(T_ARRAY, base, length);
    }

    return base;
}

/* Parse function and array declarators. Some trickery is needed to handle
 * declarations like `void (*foo)(int)`, where the inner *foo has to be 
 * traversed first, and prepended on the outer type `* (int) -> void` 
 * afterwards making it `* (int) -> void`.
 * The type returned from declarator has to be either array, function or
 * pointer, thus only need to check for type->next to find inner tail.
 */
static struct typetree *direct_declarator(
    struct typetree *base,
    const char **symbol)
{
    struct typetree *type = base;
    struct typetree *head, *tail = NULL;
    struct token ident;

    switch (peek().token) {
    case IDENTIFIER:
        ident = consume(IDENTIFIER);
        if (!symbol) {
            error("Unexpected identifier in abstract declarator.");
            exit(1);
        }
        *symbol = ident.strval;
        break;
    case '(':
        consume('(');
        type = head = tail = declarator(NULL, symbol);
        while (tail->next) {
            tail = (struct typetree *) tail->next;
        }
        consume(')');
        break;
    default:
        break;
    }

    while (peek().token == '[' || peek().token == '(') {
        switch (peek().token) {
        case '[':
            type = direct_declarator_array(base);
            break;
        case '(':
            consume('(');
            type = parameter_list(base);
            consume(')');
            break;
        default:
            assert(0);
        }
        if (tail) {
            tail->next = type;
            type = head;
        }
        base = type;
    }

    return type;
}

static struct typetree *pointer(const struct typetree *base)
{
    struct typetree *type = type_init(T_POINTER, base);

    #define set_qualifier(d) \
        if (type->qualifier & d) \
            error("Duplicate type qualifier '%s'.", peek().strval); \
        type->qualifier |= d;

    consume('*');
    while (1) {
        if (peek().token == CONST) {
            set_qualifier(Q_CONST);
        } else if (peek().token == VOLATILE) {
            set_qualifier(Q_VOLATILE);
        } else break;
        next();
    }

    #undef set_qualifier

    return type;
}

struct typetree *declarator(struct typetree *base, const char **symbol)
{
    while (peek().token == '*') {
        base = pointer(base);
    }

    return direct_declarator(base, symbol);
}


static void member_declaration_list(struct typetree *type)
{
    struct namespace ns = {0};
    struct typetree *decl_base, *decl_type;
    const char *name;

    push_scope(&ns);

    do {
        decl_base = declaration_specifiers(NULL);

        do {
            name = NULL;
            decl_type = declarator(decl_base, &name);

            if (!name) {
                error("Missing name in member declarator.");
                exit(1);
            } else if (!size_of(decl_type)) {
                error("Field '%s' has incomplete type '%t'.", name, decl_type);
                exit(1);
            } else {
                sym_add(&ns, name, decl_type, SYM_DECLARATION, LINK_NONE);
                type_add_member(type, name, decl_type);
            }

            if (peek().token == ',') {
                consume(',');
                continue;
            }
        } while (peek().token != ';');

        consume(';');
    } while (peek().token != '}');

    pop_scope(&ns);
}

static struct typetree *struct_or_union_declaration(void)
{
    struct symbol *sym = NULL;
    struct typetree *type = NULL;
    enum type kind =
        (next().token == STRUCT) ? T_STRUCT : T_UNION;

    if (peek().token == IDENTIFIER) {
        const char *name = consume(IDENTIFIER).strval;
        sym = sym_lookup(&ns_tag, name);
        if (!sym) {
            type = type_init(kind);
            sym = sym_add(&ns_tag, name, type, SYM_TYPEDEF, LINK_NONE);
        } else if (is_integer(&sym->type)) {
            error("Tag '%s' was previously declared as enum.", sym->name);
            exit(1);
        } else if (sym->type.type != kind) {
            error("Tag '%s' was previously declared as %s.",
                sym->name, (sym->type.type == T_STRUCT) ? "struct" : "union");
            exit(1);
        }

        /* Retrieve type from existing symbol, possibly providing a complete
         * definition that will be available for later declarations. Overwrites
         * existing type information from symbol table. */
        type = &sym->type;
        if (peek().token == '{' && type->size) {
            error("Redefiniton of '%s'.", sym->name);
            exit(1);
        }
    }

    if (peek().token == '{') {
        if (!type) {
            /* Anonymous structure; allocate a new standalone type,
             * not part of any symbol. */
            type = type_init(kind);
        }

        consume('{');
        member_declaration_list(type);
        assert(type->size);
        consume('}');
    }

    /* Return to the caller a copy of the root node, which can be overwritten
     * with new type qualifiers without altering the tag registration. */
    return (sym) ? type_tagged_copy(&sym->type, sym->name) : type;
}

static void enumerator_list(void)
{
    struct var val;
    struct symbol *sym;
    int enum_value = 0;

    consume('{');
    do {
        const char *name = consume(IDENTIFIER).strval;

        if (peek().token == '=') {
            consume('=');
            val = constant_expression();
            if (!is_integer(val.type)) {
                error("Implicit conversion from non-integer type in enum.");
            }
            enum_value = val.imm.i;
        }

        sym = sym_add(
            &ns_ident,
            name,
            &basic_type__int,
            SYM_ENUM_VALUE,
            LINK_NONE);
        sym->enum_value = enum_value++;

        if (peek().token != ',')
            break;
        consume(',');
    } while (peek().token != '}');
    consume('}');
}

static struct typetree *enum_declaration(void)
{
    struct typetree *type = type_init(T_SIGNED, 4);

    consume(ENUM);
    if (peek().token == IDENTIFIER) {
        struct symbol *tag = NULL;
        const char *name = consume(IDENTIFIER).strval;

        tag = sym_lookup(&ns_tag, name);
        if (!tag || tag->depth < ns_tag.current_depth) {
            tag = sym_add(&ns_tag, name, type, SYM_TYPEDEF, LINK_NONE);
        } else if (!is_integer(&tag->type)) {
            error("Tag '%s' was previously defined as aggregate type.",
                tag->name);
            exit(1);
        }

        /* Use enum_value as a sentinel to represent definition, checked on 
         * lookup to detect duplicate definitions. */
        if (peek().token == '{') {
            if (tag->enum_value) {
                error("Redefiniton of enum '%s'.", tag->name);
            }
            enumerator_list();
            tag->enum_value = 1;
        }
    } else {
        enumerator_list();
    }

    /* Result is always integer. Do not care about the actual enum definition,
     * all enums are ints and no type checking is done. */
    return type;
}

static struct typetree get_basic_type_from_specifier(unsigned short spec)
{
    switch (spec) {
    case 0x0001: /* void */
        return basic_type__void;
    case 0x0002: /* char */
    case 0x0012: /* signed char */
        return basic_type__char;
    case 0x0022: /* unsigned char */
        return basic_type__unsigned_char;
    case 0x0004: /* short */
    case 0x0014: /* signed short */
    case 0x000C: /* short int */
    case 0x001C: /* signed short int */
        return basic_type__short;
    case 0x0024: /* unsigned short */
    case 0x002C: /* unsigned short int */
        return basic_type__unsigned_short;
    case 0x0008: /* int */
    case 0x0010: /* signed */
    case 0x0018: /* signed int */
        return basic_type__int;
    case 0x0020: /* unsigned */
    case 0x0028: /* unsigned int */
        return basic_type__unsigned_int;
    case 0x0040: /* long */
    case 0x0050: /* signed long */
    case 0x0048: /* long int */
    case 0x0058: /* signed long int */
    case 0x00C0: /* long long */
    case 0x00D0: /* signed long long */
    case 0x00D8: /* signed long long int */
        return basic_type__long;
    case 0x0060: /* unsigned long */
    case 0x0068: /* unsigned long int */
    case 0x00E0: /* unsigned long long */
    case 0x00E8: /* unsigned long long int */
        return basic_type__unsigned_long;
    case 0x0100: /* float */
        return basic_type__float;
    case 0x0200: /* double */
    case 0x0240: /* long double */
        return basic_type__double;
    default:
        error("Invalid type specification.");
        exit(1); 
    }
}

/* Parse type, qualifiers and storage class. Do not assume int by default, but
 * require at least one type specifier. Storage class is returned as token
 * value, unless the provided pointer is NULL, in which case the input is parsed
 * as specifier-qualifier-list.
 */
struct typetree *declaration_specifiers(int *stc)
{
    struct typetree *type = NULL;
    struct token tok;
    int done = 0;

    /* Use a compact bit representation to hold state about declaration 
     * specifiers. Initialize storage class to sentinel value. */
    unsigned short spec = 0x0000;
    enum qualifier qual = Q_NONE;
    if (stc)       *stc =    '$';

    #define set_specifier(d) \
        if (spec & d) error("Duplicate type specifier '%s'.", tok.strval); \
        next(); spec |= d;

    #define set_qualifier(d) \
        if (qual & d) error("Duplicate type qualifier '%s'.", tok.strval); \
        next(); qual |= d;

    #define set_storage_class(t) \
        if (!stc) error("Unexpected storage class in qualifier list."); \
        else if (*stc != '$') error("Multiple storage class specifiers."); \
        next(); *stc = t;

    do {
        switch ((tok = peek()).token) {
        case VOID:      set_specifier(0x001); break;
        case CHAR:      set_specifier(0x002); break;
        case SHORT:     set_specifier(0x004); break;
        case INT:       set_specifier(0x008); break;
        case SIGNED:    set_specifier(0x010); break;
        case UNSIGNED:  set_specifier(0x020); break;
        case LONG:
            if (spec & 0x040) {
                set_specifier(0x080);
            } else {
                set_specifier(0x040);   
            }
            break;
        case FLOAT:     set_specifier(0x100); break;
        case DOUBLE:    set_specifier(0x200); break;
        case CONST:     set_qualifier(Q_CONST); break;
        case VOLATILE:  set_qualifier(Q_VOLATILE); break;
        case IDENTIFIER: {
            struct symbol *tag = sym_lookup(&ns_ident, tok.strval);
            if (tag && tag->symtype == SYM_TYPEDEF && !type) {
                consume(IDENTIFIER);
                type = type_init(T_STRUCT);
                *type = tag->type;
            } else {
                done = 1;
            }
            break;
        }
        case UNION:
        case STRUCT:
            if (!type) {
                type = struct_or_union_declaration();
            } else {
                done = 1;
            }
            break;
        case ENUM:
            if (!type) {
                type = enum_declaration();
            } else {
                done = 1;
            }
            break;
        case AUTO:
        case REGISTER:
        case STATIC:
        case EXTERN:
        case TYPEDEF:
            set_storage_class(tok.token);
            break;
        default:
            done = 1;
            break;
        }

        if (type && spec) {
            error("Invalid combination of declaration specifiers.");
            exit(1);
        }
    } while (!done);

    #undef set_specifier
    #undef set_qualifier
    #undef set_storage_class

    if (type) {
        if (qual & type->qualifier) {
            error("Duplicate type qualifier:%s%s.",
                (qual & Q_CONST) ? " const" : "",
                (qual & Q_VOLATILE) ? " volatile" : "");
        }
    } else if (spec) {
        type = type_init(T_STRUCT);
        *type = get_basic_type_from_specifier(spec);
    } else {
        error("Missing type specifier.");
        exit(1);
    }

    type->qualifier |= qual;
    return type;
}

/* Set var = 0, using simple assignment on members for composite types. This
 * rule does not consume any input, but generates a series of assignments on the
 * given variable. Point is to be able to zero initialize using normal simple
 * assignment rules, although IR can become verbose for large structures.
 */
static void zero_initialize(struct block *block, struct var target)
{
    int i;
    struct var var;
    assert(target.kind == DIRECT);

    switch (target.type->type) {
    case T_STRUCT:
    case T_UNION:
        target.type = unwrapped(target.type);
        var = target;
        for (i = 0; i < nmembers(var.type); ++i) {
            target.type = get_member(var.type, i)->type;
            target.offset = var.offset + get_member(var.type, i)->offset;
            zero_initialize(block, target);
        }
        break;
    case T_ARRAY:
        assert(target.type->size);
        var = target;
        target.type = target.type->next;
        assert(is_struct(target.type) || !target.type->next);
        for (i = 0; i < var.type->size / var.type->next->size; ++i) {
            target.offset = var.offset + i * var.type->next->size;
            zero_initialize(block, target);
        }
        break;
    case T_POINTER:
        var = var_zero(8);
        var.type = type_init(T_POINTER, &basic_type__void);
        eval_assign(block, target, var);
        break;
    case T_UNSIGNED:
    case T_SIGNED:
        var = var_zero(target.type->size);
        eval_assign(block, target, var);
        break;
    default:
        error("Invalid type to zero-initialize, was '%t'.", target.type);
        exit(1);
    }
}

static struct block *object_initializer(struct block *block, struct var target)
{
    int i,
        filled = target.offset;
    const struct typetree *type = target.type;

    assert(!is_tagged(type));

    consume('{');
    target.lvalue = 1;
    switch (type->type) {
    case T_UNION:
        /* C89 states that only the first element of a union can be
         * initialized. Zero the whole thing first if there is padding. */
        if (size_of(get_member(type, 0)->type) < type->size) {
            target.type =
                (type->size % 8) ?
                    type_init(T_ARRAY, &basic_type__char, type->size) :
                    type_init(T_ARRAY, &basic_type__long, type->size / 8);
            zero_initialize(block, target);
        }
        target.type = get_member(type, 0)->type;
        block = initializer(block, target);
        if (peek().token != '}') {
            error("Excess elements in union initializer.");
            exit(1);
        }
        break;
    case T_STRUCT:
        for (i = 0; i < nmembers(type); ++i) {
            target.type = get_member(type, i)->type;
            target.offset = filled + get_member(type, i)->offset;
            block = initializer(block, target);
            if (peek().token == ',') {
                consume(',');
            } else break;
            if (peek().token == '}') {
                break;
            }
        }
        while (++i < nmembers(type)) {
            target.type = get_member(type, i)->type;
            target.offset = filled + get_member(type, i)->offset;
            zero_initialize(block, target);
        }
        break;
    case T_ARRAY:
        target.type = type->next;
        for (i = 0; !type->size || i < type->size / size_of(type->next); ++i) {
            target.offset = filled + i * size_of(type->next);
            block = initializer(block, target);
            if (peek().token == ',') {
                consume(',');
            } else break;
            if (peek().token == '}') {
                break;
            }
        }
        if (!type->size) {
            assert(!target.symbol->type.size);
            assert(is_array(&target.symbol->type));

            /* Incomplete array type can only be in the root level of target
             * type tree, overwrite type directly in symbol. */
            ((struct symbol *) target.symbol)->type.size =
                (i + 1) * size_of(type->next);
        } else {
            while (++i < type->size / size_of(type->next)) {
                target.offset = filled + i * size_of(type->next);
                zero_initialize(block, target);
            }
        }
        break;
    default:
        error("Block initializer only apply to aggregate or union type.");
        exit(1);
    }

    consume('}');
    return block;
}

/* Parse and emit initializer code for target variable in statements such as
 * int b[] = {0, 1, 2, 3}. Generate a series of assignment operations on
 * references to target variable.
 */
static struct block *initializer(struct block *block, struct var target)
{
    assert(target.kind == DIRECT);

    /* Do not care about cv-qualifiers here. */
    target.type = unwrapped(target.type);

    if (peek().token == '{') {
        block = object_initializer(block, target);
    } else {
        block = assignment_expression(block);
        if (!target.symbol->depth && block->expr.kind != IMMEDIATE) {
            error("Initializer must be computable at load time.");
            exit(1);
        }
        if (target.kind == DIRECT && !target.type->size) {
            assert(!target.offset);
            assert(is_string(block->expr));
            assert(is_array(block->expr.type));

            /* Complete type based on string literal. Evaluation does not have
             * the required context to do this logic. */
            ((struct symbol *) target.symbol)->type.size =
                block->expr.type->size;
            target.type = block->expr.type;
        }
        eval_assign(block, target, block->expr);
    }

    return block;
}

/* C99: Define __func__ as static const char __func__[] = sym->name;
 */
static void define_builtin__func__(const char *name)
{
    struct typetree *type;
    struct symbol *sym;
    assert(ns_ident.current_depth == 1);

    /* Just add the symbol directly as a special string value. No explicit
     * assignment reflected in the IR. */
    type = type_init(T_ARRAY, &basic_type__char, strlen(name) + 1);
    sym = sym_add(&ns_ident, "__func__", type, SYM_STRING_VALUE, LINK_INTERN);
    sym->string_value = name;
}

/* Cover both external declarations, functions, and local declarations (with
 * optional initialization code) inside functions.
 */
struct block *declaration(struct block *parent)
{
    struct typetree *base;
    enum symtype symtype;
    enum linkage linkage;
    int stc = '$';

    base = declaration_specifiers(&stc);
    switch (stc) {
    case EXTERN:
        symtype = SYM_DECLARATION;
        linkage = LINK_EXTERN;
        break;
    case STATIC:
        symtype = SYM_TENTATIVE;
        linkage = LINK_INTERN;
        break;
    case TYPEDEF:
        symtype = SYM_TYPEDEF;
        linkage = LINK_NONE;
        break;
    default:
        if (!ns_ident.current_depth) {
            symtype = SYM_TENTATIVE;
            linkage = LINK_EXTERN;
        } else {
            symtype = SYM_DEFINITION;
            linkage = LINK_NONE;
        }
        break;
    }

    while (1) {
        struct definition *def;
        const char *name = NULL;
        const struct typetree *type;
        struct symbol *sym;

        type = declarator(base, &name);
        if (!name) {
            consume(';');
            return parent;
        }

        sym = sym_add(&ns_ident, name, type, symtype, linkage);
        if (ns_ident.current_depth) {
            assert(ns_ident.current_depth > 1);
            def = current_func();
            def->locals = sym_list_add(def->locals, sym);
        }

        switch (peek().token) {
        case ';':
            consume(';');
            return parent;
        case '=':
            if (sym->symtype == SYM_DECLARATION) {
                error("Extern symbol '%s' cannot be initialized.", sym->name);
                exit(1);
            }
            if (!sym->depth && sym->symtype == SYM_DEFINITION) {
                error("Symbol '%s' was already defined.", sym->name);
                exit(1);
            }
            consume('=');
            sym->symtype = SYM_DEFINITION;
            if (sym->linkage == LINK_NONE) {
                assert(parent);
                parent = initializer(parent, var_direct(sym));
            } else {
                assert(sym->depth || !parent);
                def = push_back_definition(sym);
                initializer(def->body, var_direct(sym));
            }
            assert(size_of(&sym->type) > 0);
            if (peek().token != ',') {
                consume(';');
                return parent;
            }
            break;
        case '{': {
            int i;
            if (!is_function(&sym->type) || sym->depth) {
                error("Invalid function definition.");
                exit(1);
            }
            assert(!parent);
            assert(sym->linkage != LINK_NONE);
            sym->symtype = SYM_DEFINITION;
            def = push_back_definition(sym);
            push_scope(&ns_ident);
            define_builtin__func__(sym->name);
            for (i = 0; i < nmembers(&sym->type); ++i) {
                name = get_member(&sym->type, i)->name;
                type = get_member(&sym->type, i)->type;
                symtype = SYM_DEFINITION;
                linkage = LINK_NONE;
                if (!name) {
                    error("Missing parameter name at position %d.", i + 1);
                    exit(1);
                }
                def->params = sym_list_add(def->params,
                    sym_add(&ns_ident, name, type, symtype, linkage));
            }
            parent = block(def->body);
            pop_scope(&ns_ident);
            return parent;
        }
        default:
            break;
        }
        consume(',');
    }
}

struct definition *current_func(void)
{
    int i;
    for (i = defs.len - 1; i >= defs.cur; --i)
        if (is_function(&defs.def[i].symbol->type))
            return &defs.def[i];
    assert(0);
    return NULL;
}

struct var create_var(const struct typetree *type)
{
    struct definition *def = current_func();
    struct symbol *temp = sym_create_tmp(type);
    struct var res = var_direct(temp);

    def->locals = sym_list_add(def->locals, temp);
    res.lvalue = 1;
    return res;
}

struct block *cfg_block_init(void)
{
    struct definition *def;
    struct block *block;

    block = calloc(1, sizeof(*block));
    block->label = sym_create_label();

    /* Block is owned by last added definition, also non-functions. The fallback
     * solution is to get some owner for expressiong like enum { A = 1 } foo;
     * where the constant expression is evaluated by instantiating blocks. */
    def = (defs.len) ? &defs.def[defs.len - 1] : &fallback;
    def->nodes = block_list_add(def->nodes, block);

    return block;
}

struct definition parse(void)
{
    static struct definition last_def_returned;
    struct definition def = {0};

    while (!defs.len && peek().token != END) {
        /* Parse a declaration, which can include definitions that will fill
         * up the buffer. Tentative declarations will only affect the symbol
         * table. */
        declaration(NULL);
        clear_definition(&fallback);
    }

    if (defs.cur < defs.len) {
        def = defs.def[defs.cur++];
        if (defs.cur == defs.len) {
            /* Clear definition list once the last entry is returned. No leaks
             * after everything is consumed. */
            free(defs.def);
            memset(&defs, 0, sizeof(defs));
        }
    }

    /* Clear memory allocated for previous result. Parse is called until no
     * more input can be consumed, letting us free all memory. */
    if (last_def_returned.symbol)
        clear_definition(&last_def_returned);

    last_def_returned = def;
    return def;
}

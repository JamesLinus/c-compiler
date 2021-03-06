#ifndef MACRO_H
#define MACRO_H

#include <lacc/token.h>

#include <stdlib.h>

struct macro {
    struct token name;
    enum { OBJECT_LIKE, FUNCTION_LIKE } type;

    size_t params;
    size_t size;

    /* A substitution is either a token or a parameter. */
    struct replacement {
        struct token token;
        int param;
    } *replacement;

    struct {
    	unsigned long val;
    	struct macro *next;
    } hash;
};

/* Define standard macros.
 */
void register_builtin_definitions(void);

/* Stringify a list of tokens, returning a newtoken of type STRING.
 */
struct token stringify(const struct token list[]);

/* Add macro definition. Takes ownership of any dynamically allocated
 * replacement list.
 */
void define(struct macro macro);

/* Remove macro definition corresponding to identifier. If the name has not
 * previously been defined, this is a no-op.
 */
void undef(struct token name);

/* Look up definition of identifier, or NULL if not defined.
 */
const struct macro *definition(struct token);

/* Expand a list of tokens, replacing any macro definitions. 
 */
struct token *expand(struct token *list);

/* DEBUG */
void print_list(const struct token *list);

#endif

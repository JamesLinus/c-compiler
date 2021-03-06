#ifndef DOT_H
#define DOT_H

#include <lacc/ir.h>

#include <stdio.h>

/* Output internal control flow graph intermediate representation in dot format,
 * which can be compiled for rendering.
 */
void fdotgen(FILE *stream, struct definition def);

#endif

#ifndef INTERN_H
#define INTERN_H

#include <stddef.h>

/* String interning: returns a canonical pointer for a given string.
 * Two interned strings can be compared with == instead of strcmp(). */

void intern_init(void);
void intern_free(void);

/* Intern a string. Returns canonical pointer (owned by the intern table).
 * The returned pointer remains valid until intern_free() is called. */
const char *intern(const char *s);

#endif /* INTERN_H */

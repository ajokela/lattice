#ifndef COMPLETION_H
#define COMPLETION_H

#if defined(LATTICE_HAS_EDITLINE) || defined(LATTICE_HAS_READLINE)

/* Initialize tab completion for the Lattice REPL.
 * Registers rl_attempted_completion_function with readline. */
void lattice_completion_init(void);

#else

/* No-op when readline is not available */
static inline void lattice_completion_init(void) {}

#endif /* LATTICE_HAS_EDITLINE || LATTICE_HAS_READLINE */

#endif /* COMPLETION_H */

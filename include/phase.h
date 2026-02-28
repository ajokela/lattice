#ifndef LATTICE_PHASE_H
#define LATTICE_PHASE_H

/* Phase annotation — shared between AST, compilers, and VMs.
 * Extracted so the VM can reference phase values without pulling
 * in the full AST / compiler headers. */
typedef enum { PHASE_FLUID, PHASE_CRYSTAL, PHASE_UNSPECIFIED } AstPhase;

#endif /* LATTICE_PHASE_H */

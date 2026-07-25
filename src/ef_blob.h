#ifndef ENDFIELDS_EF_BLOB_H
#define ENDFIELDS_EF_BLOB_H

#include "endfields.h"

/* ef_blob helpers exposed for test or build diagnostics. The blob API in
 * endfields.h is the public surface; this header is for internal library
 * code that needs to reuse the chain predicate. */
int ef_slot_has_overflow_chain(struct ef_db *db, const struct ef_slot *head);

/* Release any overflow slots chained off a head slot, returning them to the
 * free list. Used by ef_free_slot to clear blob storage when the head is
 * freed. */
enum ef_err ef_blob_release_chain(struct ef_db *db, uint64_t head_id, struct ef_slot *head);

#endif

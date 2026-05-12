/**
 * \file    ubi_secure_anchor.h
 * \author  Kamil Kielbasa
 * \brief   Hidden per-volume anchor PEB lifecycle for the secure backend.
 *
 * The hidden anchor is a single PEB per volume that carries the authenticated
 * upper bound of the per-volume LEB AEAD-counter floor.  It is written at
 * volume creation, rewritten when an erase would otherwise drop the sole
 * on-flash witness of that floor, and consulted at attach to reconstruct
 * the floor across a clean shutdown / power loss.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_ANCHOR_H
#define UBI_SECURE_ANCHOR_H

/* Include files -------------------------------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>

/* Forward declarations ------------------------------------------------------------------------- */

struct ubi_device;
struct ubi_volume;

/* Module interface function declarations ------------------------------------------------------- */

/**
 * \brief Allocate and commit the hidden anchor PEB for a freshly created volume.
 *
 * Consumes one free PEB, writes a zero-length LEB record with sentinel
 * \ref UBI_SECURE_INTERNAL_ANCHOR_LNUM, then commits the VID header that
 * binds the initial per-volume LEB counter floor (leb_write_counter = 1,
 * leb_total_auth_bytes = UBI_SECURE_LEB_AAD_SIZE).
 *
 * On success, \c vol->anchor_pnum holds the chosen PEB index and the
 * volume's RAM counter cache is seeded.
 *
 * Caller must hold \c ubi->mutex.
 *
 * \param[in,out] ubi UBI device.
 * \param[in,out] vol Target volume.
 *
 * \retval 0          Success.
 * \retval -EINVAL    NULL argument.
 * \retval -ENOSPC    No free PEB available.
 * \retval -EOVERFLOW VID-domain AEAD counter exhausted (KEY_ROTATE_NOW emitted).
 * \retval -EIO       Flash or crypto failure (PEB marked bad).
 */
int ubi_secure_anchor_create(struct ubi_device *ubi, struct ubi_volume *vol);

/**
 * \brief Rewrite the volume's anchor if a dirty PEB is the sole counter witness.
 *
 * Reads the dirty PEB's EC and VID headers, locates the owning volume, and
 * compares the authenticated leb_write_counter against the volume's RAM
 * cache (\c vol->cached_leb_write_counter).  If equal (the dirty PEB is
 * the only on-flash carrier of the cached floor), allocates a fresh PEB
 * and commits a new anchor record with a strictly-greater counter pair
 * before allowing the dirty PEB to be erased.  Otherwise no rewrite is
 * needed.
 *
 * Caller must hold \c ubi->mutex.
 *
 * \param[in,out] ubi        UBI device.
 * \param         dirty_pnum Physical erase block index of the dirty PEB.
 *
 * \retval 0       No rewrite needed, or anchor rewritten successfully.
 * \retval -EIO    Flash or crypto failure during rewrite.
 * \retval -ENOSPC No free PEB for anchor rewrite (caller may defer erase).
 */
int ubi_secure_anchor_rewrite_for_dirty_witness(struct ubi_device *ubi, size_t dirty_pnum);

/**
 * \brief Maintain the one-PEB reserve required for anchor rewrites.
 *
 * When the free pool drops to a single PEB while dirty PEBs remain, the
 * reserve is at risk: a future write may consume the last free PEB and
 * leave no room for an anchor rewrite.  This call attempts to recycle one
 * dirty PEB through the witness-safe path (\ref
 * ubi_secure_anchor_rewrite_for_dirty_witness followed by an erase) so
 * that \c free_peb_count is pushed back to two.
 *
 * Best-effort.  Returns silently on any error -- the caller has no
 * actionable recovery; the next user-visible write will surface
 * \c -ENOSPC if the reserve cannot be restored.
 *
 * Caller must hold \c ubi->mutex.
 *
 * \param[in,out] ubi UBI device.
 */
void ubi_secure_try_refill_reserve(struct ubi_device *ubi);

#endif /* UBI_SECURE_ANCHOR_H */

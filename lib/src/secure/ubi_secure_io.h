/**
 * \file    ubi_secure_io.h
 * \author  Kamil Kielbasa
 * \brief   Secure data-PEB I/O: encrypted EC, VID, and LEB record operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_SECURE_IO_H
#define UBI_SECURE_IO_H

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_types.h"
#include "ubi_io.h"

#include <ubi_crypto.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations ------------------------------------------------------------------------ */
struct ubi_mtd;

/* Function declarations ----------------------------------------------------------------------- */

/**
 * \brief Read and authenticate a secure EC header from a data PEB.
 *
 * On success, populates \p ec_hdr with the decrypted header and \p ec_ctx
 * with the authenticated parent context (ec value + key_version) for chained
 * VID/LEB operations.
 *
 * \param[in]  mtd         UBI MTD descriptor.
 * \param[in]  crypto_cfg  Crypto configuration (for get_key_id callback).
 * \param      peb_idx     Physical eraseblock index.
 * \param[out] ec_hdr      Decrypted EC header.
 * \param[out] ec_ctx      Authenticated EC context (for chained AAD).
 *
 * \retval 0         Success.
 * \retval -EIO      Flash or crypto failure.
 * \retval -EBADMSG  Authentication failure.
 * \retval -EINVAL   NULL argument.
 */
int ubi_secure_ec_hdr_read(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			   size_t peb_idx, struct ubi_ec_hdr *ec_hdr,
			   struct ubi_secure_ec_auth_ctx *ec_ctx);

/**
 * \brief Write an encrypted secure EC header to a data PEB.
 *
 * \param[in] mtd         UBI MTD descriptor.
 * \param[in] crypto_cfg  Crypto configuration.
 * \param     peb_idx     Physical eraseblock index.
 * \param[in] ec_hdr      EC header to encrypt and write.
 * \param     key_version Key version for encryption.
 * \param     counter     AEAD counter value.
 *
 * \retval 0       Success.
 * \retval -EIO    Flash or crypto failure.
 * \retval -EINVAL NULL argument.
 */
int ubi_secure_ec_hdr_write(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			    size_t peb_idx, const struct ubi_ec_hdr *ec_hdr, uint8_t key_version,
			    uint64_t counter);

/**
 * \brief Read and authenticate a secure VID header from a data PEB.
 *
 * On success, populates \p vid_hdr and \p vid_meta with decrypted data, and
 * \p vid_ctx with the full parent context chain for chained LEB operations.
 *
 * \param[in]  mtd         UBI MTD descriptor.
 * \param[in]  crypto_cfg  Crypto configuration.
 * \param      peb_idx     Physical eraseblock index.
 * \param[in]  ec_ctx      Authenticated EC context (parent chain for AAD).
 * \param[out] vid_hdr     Decrypted VID header.
 * \param[out] vid_meta    Decrypted VID secure metadata.
 * \param[out] vid_ctx     Authenticated VID context (for chained LEB AAD).
 *
 * \retval 0         Success.
 * \retval -EIO      Flash or crypto failure.
 * \retval -EBADMSG  Authentication failure.
 * \retval -EINVAL   NULL argument.
 */
int ubi_secure_vid_hdr_read(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			    size_t peb_idx, const struct ubi_secure_ec_auth_ctx *ec_ctx,
			    struct ubi_vid_hdr *vid_hdr, struct ubi_vid_secure_meta *vid_meta,
			    struct ubi_secure_vid_auth_ctx *vid_ctx);

/**
 * \brief Write an encrypted secure VID header to a data PEB.
 *
 * \param[in] mtd         UBI MTD descriptor.
 * \param[in] crypto_cfg  Crypto configuration.
 * \param     peb_idx     Physical eraseblock index.
 * \param[in] ec_ctx      Authenticated EC context (parent chain for AAD).
 * \param[in] vid_hdr     VID header to encrypt.
 * \param[in] vid_meta    VID secure metadata to encrypt.
 * \param     key_version Key version for encryption.
 * \param     counter     AEAD counter value.
 *
 * \retval 0       Success.
 * \retval -EIO    Flash or crypto failure.
 * \retval -EINVAL NULL argument.
 */
int ubi_secure_vid_hdr_write(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			     size_t peb_idx, const struct ubi_secure_ec_auth_ctx *ec_ctx,
			     const struct ubi_vid_hdr *vid_hdr,
			     const struct ubi_vid_secure_meta *vid_meta, uint8_t key_version,
			     uint64_t counter);

/**
 * \brief Read and authenticate a secure LEB record (single-tag) from a data PEB.
 *
 * Authenticates the full payload, then returns only the requested slice.
 *
 * \param[in]  mtd         UBI MTD descriptor.
 * \param[in]  crypto_cfg  Crypto configuration.
 * \param      peb_idx     Physical eraseblock index.
 * \param[in]  vid_ctx     Authenticated VID context (full parent chain for AAD).
 * \param      offset      Byte offset within authenticated payload to return.
 * \param[out] buf         Output buffer for the requested slice.
 * \param      len         Bytes to return.
 *
 * \retval 0         Success.
 * \retval -EIO      Flash or crypto failure.
 * \retval -EBADMSG  Authentication failure.
 * \retval -EINVAL   NULL argument or out-of-bounds slice.
 */
int ubi_secure_leb_data_read(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			     size_t peb_idx, const struct ubi_secure_vid_auth_ctx *vid_ctx,
			     size_t offset, void *buf, size_t len);

/**
 * \brief Write an encrypted secure LEB record (single-tag) to a data PEB.
 *
 * \param[in] mtd         UBI MTD descriptor.
 * \param[in] crypto_cfg  Crypto configuration.
 * \param     peb_idx     Physical eraseblock index.
 * \param[in] ec_ctx      Authenticated EC context (for AAD).
 * \param[in] vid_hdr     VID header (for AAD fields: vol_id, lnum, sqnum, data_size).
 * \param     vid_kv      VID key_version (for AAD).
 * \param[in] buf         Plaintext payload (NULL if data_size==0).
 * \param     len         Payload length (must match vid_hdr->data_size).
 * \param     key_version Key version for LEB domain.
 * \param     counter     AEAD counter value.
 *
 * \retval 0       Success.
 * \retval -EIO    Flash or crypto failure.
 * \retval -EINVAL NULL argument.
 */
int ubi_secure_leb_data_write(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			      size_t peb_idx, const struct ubi_secure_ec_auth_ctx *ec_ctx,
			      const struct ubi_vid_hdr *vid_hdr, uint8_t vid_kv, const void *buf,
			      size_t len, uint8_t key_version, uint64_t counter);

#if defined(CONFIG_UBI_CRYPTO_LEB_CHUNKED)
/**
 * \brief Read and authenticate a secure LEB record (chunked mode) from a data PEB.
 *
 * Authenticates only the chunks that cover the requested byte range (§12.3).
 * For zero-length records, returns immediately without I/O.
 *
 * \param[in]  mtd         UBI MTD descriptor.
 * \param[in]  crypto_cfg  Crypto configuration.
 * \param      peb_idx     Physical eraseblock index.
 * \param[in]  vid_ctx     Authenticated VID context (full parent chain for AAD).
 * \param      offset      Byte offset within authenticated payload to return.
 * \param[out] buf         Output buffer for the requested slice.
 * \param      len         Bytes to return.
 *
 * \retval 0         Success.
 * \retval -EIO      Flash or crypto failure.
 * \retval -EBADMSG  Authentication failure (at least one chunk).
 * \retval -EINVAL   NULL argument or out-of-bounds slice.
 */
int ubi_secure_leb_data_read_chunked(const struct ubi_mtd *mtd,
				     const struct ubi_crypto_config *crypto_cfg, size_t peb_idx,
				     const struct ubi_secure_vid_auth_ctx *vid_ctx, size_t offset,
				     void *buf, size_t len);

/**
 * \brief Write an encrypted secure LEB record (chunked mode) to a data PEB.
 *
 * Encrypts each chunk with a unique nonce (counter_base + chunk_index) and
 * writes prefix + all chunk ciphertext+tag blocks to flash.
 *
 * \param[in] mtd           UBI MTD descriptor.
 * \param[in] crypto_cfg    Crypto configuration.
 * \param     peb_idx       Physical eraseblock index.
 * \param[in] ec_ctx        Authenticated EC context (for AAD).
 * \param[in] vid_hdr       VID header (for AAD fields).
 * \param     vid_kv        VID key_version (for AAD).
 * \param[in] buf           Plaintext payload (must not be NULL, len > 0).
 * \param     len           Payload length (must match vid_hdr->data_size).
 * \param     key_version   Key version for LEB domain.
 * \param     counter_base  Base AEAD counter (chunk i uses counter_base + i).
 *
 * \retval 0       Success.
 * \retval -EIO    Flash or crypto failure.
 * \retval -EINVAL NULL argument.
 */
int ubi_secure_leb_data_write_chunked(const struct ubi_mtd *mtd,
				      const struct ubi_crypto_config *crypto_cfg, size_t peb_idx,
				      const struct ubi_secure_ec_auth_ctx *ec_ctx,
				      const struct ubi_vid_hdr *vid_hdr, uint8_t vid_kv,
				      const void *buf, size_t len, uint8_t key_version,
				      uint64_t counter_base);
#endif

/**
 * \brief Check if the VID region of a data PEB is erased.
 *
 * \param[in]  mtd       UBI MTD descriptor.
 * \param      peb_idx   Physical eraseblock index.
 * \param[out] is_erased True if VID region is all erased value.
 *
 * \retval 0       Success.
 * \retval -EIO    Flash read failure.
 * \retval -EINVAL NULL argument.
 */
int ubi_secure_vid_region_is_erased(const struct ubi_mtd *mtd, size_t peb_idx, bool *is_erased);

/**
 * \brief Check if the LEB-prefix region of a data PEB is erased.
 *
 * \param[in]  mtd       UBI MTD descriptor.
 * \param      peb_idx   Physical eraseblock index.
 * \param[out] is_erased True if LEB-prefix region is all erased value.
 *
 * \retval 0       Success.
 * \retval -EIO    Flash read failure.
 * \retval -EINVAL NULL argument.
 */
int ubi_secure_leb_prefix_is_erased(const struct ubi_mtd *mtd, size_t peb_idx, bool *is_erased);

#endif /* UBI_SECURE_IO_H */

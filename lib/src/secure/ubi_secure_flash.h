/**
 * \file    ubi_secure_flash.h
 * \author  Kamil Kielbasa
 * \brief   Shared secure-backend flash-write helper with optional test-only
 *          fault injection.
 *
 * \details Header-only static inline that wraps \ref flash_area_write and gates
 *          it through \ref ubi_test_flash_write_check_fail when
 *          \c CONFIG_UBI_TEST_FAULT_INJECTION is enabled.  Replaces the
 *          identical static \c secure_flash_write helpers previously duplicated
 *          in \c ubi_secure_io.c and \c ubi_secure_reserved.c.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_FLASH_H
#define UBI_SECURE_FLASH_H

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_plain_io.h"

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/__assert.h>

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

/* Helpers -------------------------------------------------------------------------------------- */

/**
 * \brief Write data to a flash area with optional fault injection.
 *
 * Wraps \ref flash_area_write so that test-only fault injection can
 * short-circuit the call before the underlying flash driver is touched.  In
 * production builds the function reduces to a plain \ref flash_area_write
 * forward.
 *
 * \param[in] fa     Open flash area handle.
 * \param     offset Byte offset within the flash area.
 * \param[in] data   Source buffer.
 * \param     len    Number of bytes to write.
 *
 * \return 0 on success, or negative errno on failure (e.g. -EIO when an
 *         injected fault fires).
 */
static inline int ubi_secure_flash_write(const struct flash_area *fa, off_t offset,
					 const void *data, size_t len)
{
	__ASSERT_NO_MSG(fa != NULL);
	__ASSERT_NO_MSG(data != NULL);

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (ubi_test_flash_write_check_fail()) {
		(void)offset;
		return -EIO;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

	return flash_area_write(fa, offset, data, len);
}

#endif /* UBI_SECURE_FLASH_H */

/**
 * \file    main_secure.c
 * \author  Kamil Kielbasa
 * \brief   Onboarding sample for the secure (AEAD-backed) UBI backend.
 *
 *          Mirrors the plain sample (\c main.c) but additionally wires up:
 *            - PSA Crypto initialization,
 *            - import of a SAMPLE-ONLY 16-byte root key (production must
 *              source the root key from a hardware-bound, non-exportable
 *              PSA key, e.g. a HUK-derived key inside a TF-M / PSA secure
 *              partition — never embed plaintext key material in flash
 *              images),
 *            - the four secure callbacks (\c get_key_id,
 *              \c check_freshness, \c sync_freshness, \c event_cb),
 *            - a minimal single-version key policy
 *              (\c requested_write_key_version = 1,
 *              \c allowed_key_versions = { 1 }).
 *
 *          The same volume + LEB write/read sequence as the plain sample
 *          is exercised, then the device is deinitialised and the test
 *          key is destroyed.  Build with:
 *
 *              west build -p -b native_sim sample/ \
 *                  -- -DOVERLAY_CONFIG=boards/secure.conf
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI public headers: */
#include <ubi.h>
#include <ubi_secure.h>

/* PSA Crypto: */
#include <psa/crypto.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_ID FIXED_PARTITION_ID(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/** Sample key policy: only key version 1 is in use. */
#define SAMPLE_WRITE_KEY_VERSION 1

LOG_MODULE_REGISTER(ubi_secure_sample, CONFIG_UBI_LOG_LEVEL);

/* Module types and type definitions ------------------------------------------------------------ */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

/**
 * \brief SAMPLE-ONLY 16-byte root key material.
 *
 * \warning Production deployments MUST source the root key from a
 *          hardware-bound, non-exportable PSA key (for example one
 *          derived from a Hardware Unique Key inside a TF-M or PSA
 *          secure partition).  Never embed plaintext key material in
 *          flash images.
 */
static const uint8_t sample_root_key_material[16] = {
	0x53, 0x41, 0x4d, 0x50, 0x4c, 0x45, 0x5f, 0x52,
	0x4f, 0x4f, 0x54, 0x4b, 0x45, 0x59, 0x21, 0x21,
};

/** Allowlist accepted by the secure backend at attach and on read. */
static const uint8_t sample_allowed_key_versions[] = { SAMPLE_WRITE_KEY_VERSION };

/** PSA key ID of the imported root key (assigned by \c psa_import_key). */
static psa_key_id_t sample_root_key_id = PSA_KEY_ID_NULL;

/* Static function declarations ----------------------------------------------------------------- */

/**
 * \brief Initialise PSA Crypto and import the sample root key.
 *
 * \retval 0       Success.
 * \retval -EIO    PSA initialisation or key import failed.
 */
static int sample_psa_init_and_import_key(void);

/**
 * \brief \c get_key_id callback: map a UBI key version to the PSA key ID.
 *
 * \param[in]  key_version  Key version requested by the secure backend.
 * \param[out] key_id_out   PSA key identifier for the requested version.
 *
 * \retval 0       Success.
 * \retval -ENOENT The requested key version is not provisioned.
 */
static int sample_get_key_id(uint8_t key_version, psa_key_id_t *key_id_out);

/**
 * \brief \c check_freshness callback: rollback / freshness gate at attach.
 *
 * \details Production must compare \p freshness against a trusted
 *          reference (anti-rollback counter in secure storage) and
 *          return \c UBI_SECURE_ROLLBACK_REJECT on a regression.  The
 *          sample has no trusted reference and unconditionally accepts.
 *
 * \param[in] freshness  Current on-flash freshness descriptor.
 * \param[in] user_data  Opaque pointer from \c ubi_secure_config (unused).
 *
 * \return \c UBI_SECURE_ROLLBACK_ACCEPT.
 */
static enum ubi_secure_rollback_verdict
sample_check_freshness(const struct ubi_secure_freshness *freshness, void *user_data);

/**
 * \brief \c sync_freshness callback: persist the freshness snapshot.
 *
 * \details Production must atomically persist \p freshness to the
 *          trusted reference used by \ref sample_check_freshness, and
 *          return a non-zero errno on failure (which causes the secure
 *          backend to emit \c UBI_SECURE_EVENT_FRESHNESS_SYNC_FAILURE).
 *          The sample is a no-op.
 *
 * \param[in] freshness  Freshness snapshot to persist (unused).
 * \param[in] user_data  Opaque pointer from \c ubi_secure_config (unused).
 *
 * \retval 0  Always.
 */
static int sample_sync_freshness(const struct ubi_secure_freshness *freshness, void *user_data);

/**
 * \brief \c event_cb callback: log every security-relevant event.
 *
 * \details Production should react to:
 *            - \c KEY_ROTATE_SOON / \c KEY_ROTATE_NOW — provision a
 *              replacement key version and bump
 *              \c requested_write_key_version on the next attach,
 *            - \c KEY_RETIRABLE — destroy the retired PSA key,
 *            - \c AUTH_FAILURE / \c ROLLBACK_POLICY_MISMATCH — consider
 *              entering read-only.
 *
 *          The sample only logs and continues.
 *
 * \param[in] event      Event payload from the secure backend.
 * \param[in] user_data  Opaque pointer from \c ubi_secure_config (unused).
 *
 * \return \c UBI_SECURE_EVENT_CONTINUE.
 */
static enum ubi_secure_event_verdict sample_event_cb(const struct ubi_secure_event *event,
						     void *user_data);

/**
 * \brief Stringify a \c ubi_secure_event_type for logging.
 *
 * \param[in] type  Event type to stringify.
 *
 * \return Static string literal; \c "UNKNOWN" for unrecognised values.
 */
static const char *event_type_str(enum ubi_secure_event_type type);

#if defined(CONFIG_FLASH_SIMULATOR)
/**
 * \brief Erase the UBI partition once at boot (sample-only).
 *
 * \details The native_sim flash simulator boots with RAM initialised to
 *          0x00, not the erased value the secure backend expects.  In a
 *          real deployment the partition would already carry secure
 *          metadata or be factory-blank; this helper provides the same
 *          effect for the sample on every run.  Compiled in only when
 *          \c CONFIG_FLASH_SIMULATOR=y so cross-builds for real hardware
 *          do not erase the partition on boot.
 *
 * \param[in] partition_id  Fixed-partition identifier to erase.
 *
 * \retval 0       Success.
 * \retval -errno  Flash area open / erase failure.
 */
static int sample_simulator_blank_partition(uint8_t partition_id);
#endif /* CONFIG_FLASH_SIMULATOR */

/* Static function definitions ------------------------------------------------------------------ */

static int sample_psa_init_and_import_key(void)
{
	psa_status_t status = psa_crypto_init();

	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed: %d", (int)status);
		return -EIO;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_bits(&attr, 128);

	status = psa_import_key(&attr, sample_root_key_material, sizeof(sample_root_key_material),
				&sample_root_key_id);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_import_key failed: %d", (int)status);
		return -EIO;
	}

	return 0;
}

static int sample_get_key_id(uint8_t key_version, psa_key_id_t *key_id_out)
{
	if (key_version != SAMPLE_WRITE_KEY_VERSION) {
		return -ENOENT;
	}
	*key_id_out = sample_root_key_id;
	return 0;
}

static enum ubi_secure_rollback_verdict
sample_check_freshness(const struct ubi_secure_freshness *freshness, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("[ubi-secure] check_freshness: dev_rev=%llu sqnum=%llu -> ACCEPT",
		(unsigned long long)freshness->device_revision,
		(unsigned long long)freshness->global_sqnum);
	return UBI_SECURE_ROLLBACK_ACCEPT;
}

static int sample_sync_freshness(const struct ubi_secure_freshness *freshness, void *user_data)
{
	ARG_UNUSED(freshness);
	ARG_UNUSED(user_data);
	return 0;
}

static enum ubi_secure_event_verdict sample_event_cb(const struct ubi_secure_event *event,
						     void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("[ubi-secure] event: %s", event_type_str(event->type));
	return UBI_SECURE_EVENT_CONTINUE;
}

static const char *event_type_str(enum ubi_secure_event_type type)
{
	switch (type) {
	case UBI_SECURE_EVENT_AUTH_FAILURE:
		return "AUTH_FAILURE";
	case UBI_SECURE_EVENT_FORMAT_VIOLATION:
		return "FORMAT_VIOLATION";
	case UBI_SECURE_EVENT_KEY_VERSION_NOT_ALLOWLISTED:
		return "KEY_VERSION_NOT_ALLOWLISTED";
	case UBI_SECURE_EVENT_KEY_VERSION_UNAVAILABLE:
		return "KEY_VERSION_UNAVAILABLE";
	case UBI_SECURE_EVENT_ROLLBACK_POLICY_MISMATCH:
		return "ROLLBACK_POLICY_MISMATCH";
	case UBI_SECURE_EVENT_FRESHNESS_SYNC_FAILURE:
		return "FRESHNESS_SYNC_FAILURE";
	case UBI_SECURE_EVENT_RNG_FAILURE:
		return "RNG_FAILURE";
	case UBI_SECURE_EVENT_KEY_ROTATE_SOON:
		return "KEY_ROTATE_SOON";
	case UBI_SECURE_EVENT_KEY_ROTATE_NOW:
		return "KEY_ROTATE_NOW";
	case UBI_SECURE_EVENT_KEY_RETIRABLE:
		return "KEY_RETIRABLE";
	default:
		return "UNKNOWN";
	}
}

#if defined(CONFIG_FLASH_SIMULATOR)
static int sample_simulator_blank_partition(uint8_t partition_id)
{
	const struct flash_area *fa = NULL;
	int ret = flash_area_open(partition_id, &fa);

	if (ret != 0) {
		return ret;
	}

	ret = flash_area_erase(fa, 0, UBI_PARTITION_SIZE);
	flash_area_close(fa);
	return ret;
}
#endif /* CONFIG_FLASH_SIMULATOR */

/* Module interface function definitions -------------------------------------------------------- */

int main(void)
{
	int ret = -1;
	int dret;

	LOG_INF("Hello world zephyr-ubi secure sample!");

	ret = sample_psa_init_and_import_key();
	if (ret != 0) {
		LOG_ERR("PSA init / key import failure: %d", ret);
		return ret;
	}

	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	struct flash_pages_info page_info = { 0 };

	ret = flash_get_page_info_by_offs(flash_dev, 0, &page_info);
	if (ret != 0) {
		LOG_ERR("Get page info failure: %d", ret);
		goto destroy_key;
	}

	struct ubi_flash_desc flash = {
		.partition_id = UBI_PARTITION_ID,
		.erase_block_size = page_info.size,
		.write_block_size = flash_get_write_block_size(flash_dev),
	};

#if defined(CONFIG_FLASH_SIMULATOR)
	ret = sample_simulator_blank_partition(flash.partition_id);
	if (ret != 0) {
		LOG_ERR("Sample-only flash erase failure: %d", ret);
		goto destroy_key;
	}
#endif

	const struct ubi_secure_config secure_cfg = {
		.policy = {
			.requested_write_key_version = SAMPLE_WRITE_KEY_VERSION,
			.allowed_key_versions = sample_allowed_key_versions,
			.allowed_key_versions_len = ARRAY_SIZE(sample_allowed_key_versions),
		},
		.get_key_id = sample_get_key_id,
		.check_freshness = sample_check_freshness,
		.sync_freshness = sample_sync_freshness,
		.event_cb = sample_event_cb,
		.user_data = NULL,
	};

	struct ubi_device *ubi = NULL;

	ret = ubi_device_init(&flash, &secure_cfg, &ubi);
	if (ret != 0) {
		LOG_ERR("UBI secure initialization failure: %d", ret);
		goto destroy_key;
	}

	struct ubi_volume_config vol_cfg = {
		.name = "demo",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	ret = ubi_volume_create(ubi, &vol_cfg, &vol_id);
	if (ret != 0) {
		LOG_ERR("Volume create failure: %d", ret);
		goto deinit;
	}

	const char wdata[] = "Hello, secure UBI!";

	ret = ubi_leb_write(ubi, vol_id, 0, wdata, sizeof(wdata));
	if (ret != 0) {
		LOG_ERR("LEB write failure: %d", ret);
		goto deinit;
	}

	char rdata[64] = { 0 };

	ret = ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(wdata));
	if (ret != 0) {
		LOG_ERR("LEB read failure: %d", ret);
		goto deinit;
	}

	LOG_INF("Read back: %s", rdata);

	struct ubi_device_info dev_info = { 0 };

	ret = ubi_device_get_info(ubi, &dev_info);
	if (ret != 0) {
		LOG_ERR("Device get info failure: %d", ret);
		goto deinit;
	}

	LOG_INF("Volumes: %zu, Free PEBs: %zu, Read-only-degraded: %s", dev_info.volume_count,
		dev_info.free_peb_count, dev_info.read_only_degraded ? "yes" : "no");

deinit:
	dret = ubi_device_deinit(ubi);
	if (dret != 0) {
		LOG_ERR("UBI deinitialization failure: %d", dret);
		if (ret == 0) {
			ret = dret;
		}
	}

destroy_key:
	(void)psa_destroy_key(sample_root_key_id);
	sample_root_key_id = PSA_KEY_ID_NULL;

	return ret;
}

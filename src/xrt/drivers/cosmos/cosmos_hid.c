// Copyright 2026, LunaticCat.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the Cosmos HID interface.
 * @author LunaticCat <lunareredwood@gmail.com>
 * @ingroup drv_cosmos
 */

#include "os/os_hid.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_file.h"
#include "util/u_time.h"

#include "math/m_api.h"

#include "xrt/xrt_byte_order.h"

#include "cosmos_hid.h"

#include <zlib.h>

#include "cosmos_config.h"


DEBUG_GET_ONCE_LOG_OPTION(cosmos_log, "COSMOS_LOG", U_LOGGING_WARN)

#define COSMOS_TRACE(cosmos, ...) U_LOG_IFL_T(cosmos->log_level, __VA_ARGS__)
#define COSMOS_DEBUG(cosmos, ...) U_LOG_IFL_D(cosmos->log_level, __VA_ARGS__)
#define COSMOS_INFO(cosmos, ...) U_LOG_IFL_I(cosmos->log_level, __VA_ARGS__)
#define COSMOS_WARN(cosmos, ...) U_LOG_IFL_W(cosmos->log_level, __VA_ARGS__)
#define COSMOS_ERROR(cosmos, ...) U_LOG_IFL_E(cosmos->log_level, __VA_ARGS__)

#define COSMOS_HID_BUF_SIZE 65
#define COSMOS_CONFIG_FOLDER "vive_cosmos"
#define COSMOS_CONFIG_CHUNK_SIZE 56

struct cosmos_hid
{
	enum u_logging_level log_level;

	struct os_hid_device *hid;

	struct cosmos_config config;

	char serial[COSMOS_HID_BUF_SIZE];
};

// The Cosmos mainboard stalls (EPIPE) when feature-report transactions are issued
// back-to-back; a short pause between each SET/GET_FEATURE is required for the config
// read to complete reliably. 5 ms is comfortably robust (verified down to ~1 ms).
#define COSMOS_HID_XFER_DELAY_NS (5 * U_TIME_1MS_IN_NS)

#pragma pack(push, 1)

struct cosmos_feature_header
{
	uint8_t id;
	__le16 sub_id;
	uint8_t size;
};

#pragma pack(pop)

static bool
cosmos_hid_set_display_power(struct cosmos_hid *cosmos, bool power_on)
{
	uint8_t buffer[COSMOS_HID_BUF_SIZE] = {0x00, 0x78, 0x29, 0x38, 0x00, 0x80};

	buffer[47] = power_on ? 0x01 : 0x00;

	os_nanosleep(COSMOS_HID_XFER_DELAY_NS);

	int ret = os_hid_set_feature(cosmos->hid, buffer, sizeof(buffer));
	if (ret < 0) {
		COSMOS_ERROR(cosmos, "Failed to send display power toggle.");
		return false;
	}

	return true;
}

static int
cosmos_get_config_size(struct cosmos_hid *cosmos, uint32_t *out_size)
{
	uint8_t buf[COSMOS_HID_BUF_SIZE] = {0};

	os_nanosleep(COSMOS_HID_XFER_DELAY_NS);

	int ret = os_hid_get_feature(cosmos->hid, 0x10, buf, sizeof(buf));
	if (ret < 0) {
		COSMOS_ERROR(cosmos, "Failed to read config size (0x10).");
		return ret;
	}

	__le32 size;
	memcpy(&size, &buf[4], sizeof(uint32_t));
	*out_size = __le32_to_cpu(size);

	if (*out_size == 0) {
		COSMOS_ERROR(cosmos, "Headset reported a config size of 0 bytes.");
		return -1;
	}

	return 0;
}

static int
cosmos_read_config(struct cosmos_hid *cosmos, char **out_config, size_t *out_size)
{
	uint8_t prep_req[COSMOS_HID_BUF_SIZE] = {0x00, 0x10, 0x00, 0x0b, 0xff};
	snprintf((char *)&prep_req[5], sizeof(prep_req) - 5, "HMD_JSON.gz");

	os_nanosleep(COSMOS_HID_XFER_DELAY_NS);

	if (os_hid_set_feature(cosmos->hid, prep_req, sizeof(prep_req)) < 0) {
		COSMOS_ERROR(cosmos, "Failed to send JSON prep payload.");
		return -1;
	}

	uint32_t file_size = 0;
	int ret = cosmos_get_config_size(cosmos, &file_size);
	if (ret < 0) {
		return ret;
	}

	COSMOS_DEBUG(cosmos, "Headset reports compressed config is %u bytes.", file_size);

	uint8_t *compressed_data = U_TYPED_ARRAY_CALLOC(uint8_t, file_size);
	uint32_t offset = 0;

	while (offset < file_size) {
		uint32_t chunk_size = MIN(COSMOS_CONFIG_CHUNK_SIZE, file_size - offset);

		uint8_t req[COSMOS_HID_BUF_SIZE] = {0x00, 0x11, 0x00, 0x08, 0x80};

		__le32 le_offset = __cpu_to_le32(offset);
		__le32 le_chunk = __cpu_to_le32(chunk_size);
		memcpy(&req[5], &le_offset, sizeof(__le32));
		memcpy(&req[9], &le_chunk, sizeof(__le32));

		os_nanosleep(COSMOS_HID_XFER_DELAY_NS);

		ret = os_hid_set_feature(cosmos->hid, req, sizeof(req));
		if (ret < 0) {
			COSMOS_ERROR(cosmos, "Failed to send chunk request at offset %u", offset);
			free(compressed_data);
			return -1;
		}

		uint8_t resp[COSMOS_HID_BUF_SIZE] = {0x11};
		ret = os_hid_get_feature(cosmos->hid, 0x11, resp, sizeof(resp));
		if (ret < 0) {
			COSMOS_ERROR(cosmos, "Failed to read chunk at offset %u", offset);
			free(compressed_data);
			return -1;
		}

		os_nanosleep(COSMOS_HID_XFER_DELAY_NS);

		memcpy(compressed_data + offset, &resp[4], chunk_size);
		offset += chunk_size;
	}

	COSMOS_DEBUG(cosmos, "Finished downloading %u bytes. Decompressing...", file_size);

	if (file_size < 3 || compressed_data[0] != 0x1F || compressed_data[1] != 0x8B || compressed_data[2] != 0x08) {
		COSMOS_ERROR(cosmos, "Invalid or missing GZIP header in downloaded stream.");
		free(compressed_data);
		return -1;
	}

	size_t uncompressed_max = 32768;
	char *uncompressed_data = U_TYPED_ARRAY_CALLOC(char, uncompressed_max);

	z_stream zs = {
	    .next_in = compressed_data,
	    .avail_in = file_size,
	    .next_out = (Bytef *)uncompressed_data,
	    .avail_out = uncompressed_max - 1,
	    .zalloc = Z_NULL,
	    .zfree = Z_NULL,
	    .opaque = Z_NULL,
	};

	if (inflateInit2(&zs, 15 + 16) != Z_OK) {
		COSMOS_ERROR(cosmos, "Failed to initialize zlib for decompression.");
		free(compressed_data);
		free(uncompressed_data);
		return -1;
	}

	int zret = inflate(&zs, Z_FINISH);
	(void)inflateEnd(&zs); // ignore errors, we're closing the stream anyway

	if (zret != Z_STREAM_END && zret != Z_OK) {
		COSMOS_ERROR(cosmos, "Failed to decompress JSON payload (zret: %d).", zret);
		free(compressed_data);
		free(uncompressed_data);
		return -1;
	}

	*out_size = zs.total_out;
	uncompressed_data[zs.total_out] = '\0';
	*out_config = uncompressed_data;

	COSMOS_INFO(cosmos, "Extracted and decompressed HMD_JSON.json");

	free(compressed_data);
	return 0;
}

static int
cosmos_load_config(struct cosmos_hid *cosmos)
{
	char config_filename[128];
	snprintf(config_filename, sizeof(config_filename), "%s.json", cosmos->serial);

	FILE *file = u_file_open_file_in_config_dir_subpath(COSMOS_CONFIG_FOLDER, config_filename, "r");
	if (file != NULL) {
		size_t config_size = 0;
		char *contents = u_file_read_content(file, &config_size);
		fclose(file);

		if (contents != NULL) {
			if (cosmos_config_parse(contents, config_size, &cosmos->config)) {
				COSMOS_DEBUG(cosmos, "Loaded Cosmos config from cache: %s", config_filename);
				free(contents);
				return 0;
			}
			COSMOS_ERROR(cosmos, "Failed to parse cached config file contents. Falling back to device.");
			free(contents);
		} else {
			COSMOS_ERROR(cosmos, "Failed to read cached config file contents.");
		}
	}

	char *config = NULL;
	size_t config_size = 0;

	for (int attempt = 0; attempt < 3; attempt++) {
		COSMOS_TRACE(cosmos, "Reading config from device, attempt %d...", attempt + 1);

		int ret = cosmos_read_config(cosmos, &config, &config_size);
		if (ret >= 0 && config != NULL) {
			COSMOS_DEBUG(cosmos, "Read Cosmos config from device (%zu bytes)", config_size);
			if (!cosmos_config_parse(config, config_size, &cosmos->config)) {
				COSMOS_ERROR(cosmos, "Failed to parse config, trying again...");
				free(config);
				config = NULL;
			} else {
				COSMOS_DEBUG(cosmos, "Parsed Cosmos config successfully.");
				break;
			}
		} else {
			COSMOS_WARN(cosmos, "Failed to read config from device on attempt %d.", attempt + 1);
		}
	}

	if (config == NULL) {
		COSMOS_ERROR(cosmos, "Failed to read config from device after retries.");
		return -1;
	}

	int ret = 0;

	file = u_file_open_file_in_config_dir_subpath(COSMOS_CONFIG_FOLDER, config_filename, "wb");
	if (file == NULL) {
		COSMOS_WARN(cosmos, "Failed to open config file for writing: %s, this is non-fatal", config_filename);
		goto load_config_end;
	}

	size_t to_write = config_size;
	while (to_write > 0) {
		size_t written = fwrite(config + (config_size - to_write), 1, to_write, file);
		if (written == 0) {
			COSMOS_WARN(cosmos, "Failed to write to config file: %s, this is non-fatal", config_filename);
			break;
		}
		to_write -= written;
	}

	fflush(file);
	fclose(file);

	COSMOS_DEBUG(cosmos, "Wrote Cosmos config file to cache.");

load_config_end:
	free(config);

	return ret;
}

int
cosmos_hid_open(struct os_hid_device *hid_dev, struct cosmos_hid **out_hid)
{
	int ret;

	struct cosmos_hid *cosmos = U_TYPED_CALLOC(struct cosmos_hid);

	cosmos->log_level = debug_get_log_option_cosmos_log();
	cosmos->hid = hid_dev;

	COSMOS_INFO(cosmos, "Opened Cosmos HID device.");

	// TODO No proper serial yet
	snprintf(cosmos->serial, sizeof(cosmos->serial), "cosmos_test");
	COSMOS_DEBUG(cosmos, "Using hardcoded serial: %s", cosmos->serial);

	if (!cosmos_hid_set_display_power(cosmos, true)) {
		COSMOS_WARN(cosmos, "Failed to turn on displays.");
	}

	ret = cosmos_load_config(cosmos);
	if (ret < 0) {
		COSMOS_ERROR(cosmos, "Failed to load config.");
		cosmos_hid_destroy(cosmos);
		return ret;
	}

	*out_hid = cosmos;

	return 0;
}

struct cosmos_config *
cosmos_get_config(struct cosmos_hid *cosmos)
{
	assert(cosmos != NULL);

	return &cosmos->config;
}

void
cosmos_hid_destroy(struct cosmos_hid *cosmos)
{
	if (cosmos->hid != NULL) {
		(void)cosmos_hid_set_display_power(cosmos, false); // Ignoring error, we're exiting

		os_hid_destroy(cosmos->hid);
		cosmos->hid = NULL;
	}

	COSMOS_TRACE(cosmos, "Destroyed Cosmos HID device.");

	free(cosmos);
}

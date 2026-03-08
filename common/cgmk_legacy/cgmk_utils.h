#pragma once

#define _CRT_SECURE_NO_WARNINGS
#pragma warning(disable : 4996)

#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strlen
#include <stdint.h>

struct desc_data {
	void *buf;
	size_t buf_size;
	uint32_t mkey;
	uint16_t vhca_id;
	char access_key[256];
	size_t access_key_sz;	
};

//========================================================================================================================================
/*                                    vhca_id mkey     buf_addr         buf_size access_key							 */
#define BUFF_DESC_STRING_LENGTH (sizeof "0102:01020304:0102030405060708:01020304:0102030405060708091011121314151617181920212223242526272829303132")

static inline int
serialize_desc_data(struct desc_data *data, char* desc_str, size_t desc_length)
{
    int ret;

    if (desc_length < BUFF_DESC_STRING_LENGTH) {
        fprintf(stderr,
                "desc string size (%lu) is less than required (%lu) for sending cross gvmi attributes\n",
                (unsigned long)desc_length,
                (unsigned long)BUFF_DESC_STRING_LENGTH);
        return 0;
    }

    ret = sprintf(desc_str, "%04x:%08x:%016llx:%08lx:",
                  data->vhca_id,
                  data->mkey,
                  (unsigned long long)(uintptr_t)data->buf,
                  (unsigned long)data->buf_size);

    memcpy(desc_str + ret, data->access_key, data->access_key_sz);
    desc_str[ret + data->access_key_sz] = '\0';

    return 1;
}

static inline int
deserialize_desc_data(const char *str, size_t str_length, struct desc_data *data)
{
    unsigned long mkey_tmp;
    unsigned long long buf_tmp;
    unsigned long buf_size_tmp;
    int ret;

    if (!str || !data) {
        return 0;
    }

    memset(data, 0, sizeof(*data));

    ret = sscanf(str, "%hx:%lx:%llx:%lx",
                 &data->vhca_id,
                 &mkey_tmp,
                 &buf_tmp,
                 &buf_size_tmp);

    if (ret < 4) {
        fprintf(stderr, "failed to deserialize desc_data\n");
        return 0;
    }

    data->mkey = (uint32_t)mkey_tmp;
    data->buf = (void *)(uintptr_t)buf_tmp;
    data->buf_size = (size_t)buf_size_tmp;

    const char *last_colon = strrchr(str, ':');
    if (!last_colon) {
        fprintf(stderr, "failed to find access key in desc string\n");
        return 0;
    }

    last_colon++;
    data->access_key_sz = strnlen(last_colon, str_length - (size_t)(last_colon - str));

    if (data->access_key_sz >= sizeof(data->access_key)) {
        fprintf(stderr, "access key too large\n");
        return 0;
    }

    memcpy(data->access_key, last_colon, data->access_key_sz);
    data->access_key[data->access_key_sz] = '\0';

    return 1;
}

int
sign_buffer(void *buf, size_t buf_size) {
	uint8_t *buffer = (uint8_t*)buf;
	if (buf_size < 4) {
		fprintf(stderr, "Buffer size is too small to sign.\n");
		return -1;
	}
	*(uint32_t *)&buffer[0] = htonl((uint32_t)0xABCDEFFF);
	return 0;
}

int
verify_signature(void *buf, size_t buf_size) {
	uint8_t *buffer = (uint8_t*)buf;
	if (buf_size < 4) {
		fprintf(stderr, "Buffer size is too small to verify signature.\n");
		return -1;
	}
	printf("Buffer bits: %02x %02x %02x %02x\n",
		buffer[0], buffer[1], buffer[2], buffer[3]);
	if (buffer[0] != 0xAB ||
		buffer[1] != 0xCD ||
		buffer[2] != 0xEF ||
		buffer[3] != 0xFF) {
		fprintf(stderr, "Buffer is not contains the signature.\n");
		return -1;
	}

	printf("Buffer is verified.\n");

	return 0;
}

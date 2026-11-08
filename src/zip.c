#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <raylib.h>
#include "zip.h"

#define ZIP_MAX_SIZE (64 * 1024 * 1024)

static unsigned int le16(const unsigned char* p)
{
	return p[0] | p[1] << 8;
}

static unsigned int le32(const unsigned char* p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24;
}

static bool rom_name(const char* name, int length)
{
	const char* extensions[] = { ".nes", ".nsf", ".nsfe", ".fds" };
	for (size_t i=0; i<sizeof(extensions)/sizeof(extensions[0]); i++) {
		int e = strlen(extensions[i]);
		if (length <= e)
			continue;
		bool match = true;
		for (int k=0; k<e; k++) {
			char c = name[length - e + k];
			if ('A' <= c && c <= 'Z')
				c += 32;
			if (c != extensions[i][k]) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

// reads the central directory at the end of the zip and inflates the first rom in it
unsigned char* zip_extract_rom(const char* path, int* size, char* name, int name_size)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	if (length < 22 || ZIP_MAX_SIZE < length) {
		fclose(file);
		return NULL;
	}
	unsigned char* zip = malloc(length);
	fseek(file, 0, SEEK_SET);
	bool read = zip && fread(zip, 1, length, file) == (size_t)length;
	fclose(file);
	if (!read) {
		free(zip);
		return NULL;
	}
	unsigned char* result = NULL;

	// the end record sits in the last 22 bytes plus up to 64KB of comment
	long end = -1;
	for (long i=length - 22; 0 <= i && length - 22 - 65535 <= i; i--) {
		if (le32(zip + i) == 0x06054B50) {
			end = i;
			break;
		}
	}
	if (end < 0) {
		printf("zip has no central directory\n");
		goto done;
	}
	int entries = le16(zip + end + 10);
	long at = le32(zip + end + 16);
	for (int e=0; e<entries; e++) {
		if (length < at + 46 || le32(zip + at) != 0x02014B50)
			break;
		int flags = le16(zip + at + 8);
		int method = le16(zip + at + 10);
		unsigned int crc = le32(zip + at + 16);
		long packed = le32(zip + at + 20);
		long unpacked = le32(zip + at + 24);
		int name_length = le16(zip + at + 28);
		int extra_length = le16(zip + at + 30);
		int comment_length = le16(zip + at + 32);
		long local = le32(zip + at + 42);
		const char* entry_name = (const char*)zip + at + 46;
		at += 46 + name_length + extra_length + comment_length;
		if (length < at || !rom_name(entry_name, name_length))
			continue;
		if (flags & 0x01) {
			printf("zip entry is encrypted\n");
			continue;
		}
		if (length < local + 30 || le32(zip + local) != 0x04034B50)
			continue;
		long data = local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
		if (length < data + packed || ZIP_MAX_SIZE < unpacked)
			continue;

		unsigned char* out = NULL;
		int out_size = 0;
		if (method == 0) {
			out = malloc(packed ? packed : 1);
			if (out) {
				memcpy(out, zip + data, packed);
				out_size = packed;
			}
		} else if (method == 8) {
			unsigned char* inflated = DecompressData(zip + data, packed, &out_size);
			if (inflated) {
				out = malloc(out_size ? out_size : 1);
				if (out)
					memcpy(out, inflated, out_size);
				MemFree(inflated);
			}
		} else {
			printf("zip entry uses an unsupported compression method %d\n", method);
			continue;
		}
		if (out == NULL || out_size != unpacked || ComputeCRC32(out, out_size) != crc) {
			printf("zip entry is damaged\n");
			free(out);
			continue;
		}
		int n = name_length < name_size - 1 ? name_length : name_size - 1;
		memcpy(name, entry_name, n);
		name[n] = '\0';
		*size = out_size;
		result = out;
		break;
	}
	if (result == NULL)
		printf("no rom found in the zip\n");
done:
	free(zip);
	return result;
}

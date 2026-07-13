/**
 * @file arrow_c_data.h
 * @brief Dependency-free declarations for the Apache Arrow C Data Interface.
 *
 * These declarations follow the stable Arrow C Data Interface ABI. ZYX does
 * not require or link the Apache Arrow runtime; callers may populate these
 * structures directly or export them from any Arrow-compatible library.
 *
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <stdint.h>

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#ifdef __cplusplus
extern "C" {
#endif

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
	const char *format;
	const char *name;
	const char *metadata;
	int64_t flags;
	int64_t n_children;
	struct ArrowSchema **children;
	struct ArrowSchema *dictionary;
	void (*release)(struct ArrowSchema *);
	void *private_data;
};

struct ArrowArray {
	int64_t length;
	int64_t null_count;
	int64_t offset;
	int64_t n_buffers;
	int64_t n_children;
	const void **buffers;
	struct ArrowArray **children;
	struct ArrowArray *dictionary;
	void (*release)(struct ArrowArray *);
	void *private_data;
};

#ifdef __cplusplus
}
#endif

#endif /* ARROW_C_DATA_INTERFACE */

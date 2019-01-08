#pragma once
#include "skynet_malloc.h"

struct Array {
	char buf[4096];
	char *data;
	size_t len;
};

inline char *AllocArray(struct Array *arr, size_t l) {
	if (l > sizeof(arr->buf)) {
		arr->data = (char *)skynet_malloc(l);
	} else {
		arr->data = arr->buf;
	}
	arr->len = l;
	return arr->data;
}

inline void FreeArray(struct Array *arr) {
	if (arr->len > sizeof(arr->buf)) {
		skynet_free(arr->data);
	}
}


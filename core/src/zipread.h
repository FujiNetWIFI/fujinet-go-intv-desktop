/* A minimal zip reader (stored and deflated entries) over zlib. */
#ifndef ZIPREAD_H
#define ZIPREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zipread zipread;

zipread *zipread_open(const char *path);    /* NULL if not a zip */
void zipread_close(zipread *z);
int zipread_count(zipread *z);
const char *zipread_name(zipread *z, int i);
uint32_t zipread_size(zipread *z, int i);
/* Extracts entry i into a malloc'd buffer (caller frees). 0 on success. */
int zipread_extract(zipread *z, int i, uint8_t **data, uint32_t *size, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif

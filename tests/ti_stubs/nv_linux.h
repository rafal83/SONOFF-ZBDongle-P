/* Test-only NOR backend for the real TI NVOCMP source's host API. */
#ifndef TEST_NV_LINUX_H
#define TEST_NV_LINUX_H
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
typedef void *NVS_Handle;
typedef struct { uint32_t regionSize, sectorSize; } NVS_Attrs;
#define NVS_HANDLE ((void *)1)
#define NVOCMP_FLASHACCESS(err) ((void)(err));
#define NVOCMP_ALERT(cond, message) ((void)(cond));
#define NVOCMP_ASSERT(cond, message) assert(cond);
void NV_LINUX_init(void);
void NV_LINUX_save(void);
void NV_LINUX_read(uint8_t page, uint16_t offset, uint8_t *buffer, uint16_t length);
int NV_LINUX_write(uint8_t page, uint16_t offset, uint8_t *buffer, uint16_t length);
int NV_LINUX_erase(uint8_t page);
#endif

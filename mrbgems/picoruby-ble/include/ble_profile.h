#ifndef PICORUBY_BLE_PROFILE_H_
#define PICORUBY_BLE_PROFILE_H_

#include <stddef.h>
#include <stdint.h>

static inline int
BLE_profile_extent(const uint8_t *db, size_t db_len, size_t *extent)
{
  if (db == NULL) {
    if (db_len != 0) return -1;
    if (extent) *extent = 0;
    return 0;
  }
  if (db_len < 1 || db[0] != 1) return -1;
  size_t pos = 1;
  while (pos <= db_len && db_len - pos >= 2) {
    size_t entry_size = (size_t)db[pos] | ((size_t)db[pos + 1] << 8);
    if (entry_size == 0) {
      if (extent) *extent = pos + 2;
      return 0;
    }
    if (entry_size < 8 || entry_size > db_len - pos) return -1;
    uint16_t flags = (uint16_t)db[pos + 2] | ((uint16_t)db[pos + 3] << 8);
    if ((flags & 0x0200) && entry_size < 22) return -1;
    pos += entry_size;
  }
  return -1;
}

#endif

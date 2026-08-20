#ifndef DYNARR_H
#define DYNARR_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef DYNARR_DISASSERT
#define da_assert(cond)
#else
#include <assert.h>
#define da_assert(cond) assert(cond)
#endif

#define da_define(type, name) \
  typedef struct name {       \
    uint32_t size;            \
    uint32_t capacity;        \
    type* data;               \
  } name

#define da_grow_capacity(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)

#define da_realloc(ptr, old_size, new_size)             \
  ({                                                    \
    void* _ptr = NULL;                                  \
    if ((new_size) == 0) {                              \
      free(ptr);                                        \
      _ptr = NULL;                                      \
    } else if ((old_size) == 0) {                       \
      _ptr = malloc((new_size) * sizeof(*ptr));         \
    } else {                                            \
      _ptr = realloc((ptr), (new_size) * sizeof(*ptr)); \
    }                                                   \
    _ptr;                                               \
  })

#define da_free(da) da_realloc((da)->data, (da)->capacity, 0)

#define da_reserve(da, rsvsz)                                      \
  do {                                                             \
    if ((rsvsz) > (da)->capacity) {                                \
      uint32_t _newsz = (da)->capacity;                            \
      while ((rsvsz) > _newsz) {                                   \
        _newsz = da_grow_capacity(_newsz);                         \
      }                                                            \
      (da)->data = da_realloc((da)->data, (da)->capacity, _newsz); \
      da_assert((da)->data && "Failed to allocate memory");        \
      (da)->capacity = _newsz;                                     \
    }                                                              \
  } while (0)

#define da_reserve_exact(da, rsvsz)                                 \
  do {                                                              \
    if ((rsvsz) > (da)->capacity) {                                 \
      (da)->data = da_realloc((da)->data, (da)->capacity, (rsvsz)); \
      da_assert((da)->data && "Failed to allocate memory");         \
      (da)->capacity = (rsvsz);                                     \
    }                                                               \
  } while (0)

#define da_append(da, item)            \
  do {                                 \
    da_reserve((da), (da)->size + 1);  \
    (da)->data[(da)->size++] = (item); \
  } while (0)

#define da_nappend(da, items, itemsz)         \
  do {                                        \
    da_reserve((da), (da)->size + (itemsz));  \
    memcpy((da)->data + (da)->size, (items),  \
           (itemsz) * sizeof((da)->data[0])); \
    (da)->size += (itemsz);                   \
  } while (0)

#define da_resize(da, newsz)   \
  do {                         \
    da_reserve((da), (newsz)); \
    (da)->size = (newsz);      \
  } while (0)

#define da_resize_exact(da, newsz)   \
  do {                               \
    da_reserve_exact((da), (newsz)); \
    (da)->size = (newsz);            \
  } while (0)

#define da_first(da) ((da)->data[da_assert((da)->size), 0])

#define da_last(da) ((da)->data[da_assert((da)->size), (da)->size - 1])

#define da_remove_unordered(da, i) \
  do {                             \
    da_assert((i) < (da)->size);   \
    (da)->data[i] = da_last(da);   \
    (da)->size--;                  \
  } while (0)

#define da_foreach(it, da) \
  for (typeof((da)->data) it = (da)->data; it < (da)->data + (da)->size; it++)

#endif

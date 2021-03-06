#include "hash.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Minimum HASHMAP size allowed
#define MIN_HASHMAP_SIZE 8

static size_t get_probe_length(hashmap_t* map, size_t hash, size_t current,
  size_t mask)
{
  return (current + map->size - (hash & mask)) & mask;
}

static void* search(hashmap_t* map, size_t* pos, void* key, size_t hash,
  cmp_fn cmp, size_t* probe_length, size_t* oi_probe_length)
{
  size_t mask = map->size - 1;

  size_t p_length = *probe_length;
  size_t oi_p_length = 0;
  size_t index = ((hash & mask) + p_length) & mask;
  void* elem;
  size_t elem_hash;

  for(size_t i = 0; i <= mask; i++)
  {
    elem = map->buckets[index].ptr;
    elem_hash = map->buckets[index].hash;

    if(elem == NULL)
    {
      // empty bucket found
      *pos = index;
      *probe_length = p_length;
      return NULL;
    } else if(p_length >
        (oi_p_length = get_probe_length(map, elem_hash, index, mask))) {
      // our probe length is greater than the elements probe length
      // we would normally have swapped so return this position
      *pos = index;
      *probe_length = p_length;
      *oi_probe_length = oi_p_length;
      return NULL;
    } else if((hash == elem_hash) && cmp(key, elem)) {
      // element found
      *pos = index;
      *probe_length = p_length;
      return elem;
    }

    index = (index + 1) & mask;
    p_length++;
  }

  *pos = map->size;
  *probe_length = p_length;
  return NULL;
}

static void resize(hashmap_t* map, cmp_fn cmp, alloc_fn alloc,
  free_size_fn fr)
{
  size_t s = map->size;
  size_t c = map->count;
  hashmap_entry_t* b = map->buckets;
  bitmap_t* old_item_bitmap = map->item_bitmap;
  void* curr = NULL;

  map->count = 0;
  map->size = (s < MIN_HASHMAP_SIZE) ? MIN_HASHMAP_SIZE : s << 3;

  // use a single memory allocation to exploit spatial memory/cache locality
  size_t bitmap_size = map->size/HASHMAP_BITMAP_TYPE_SIZE +
    (map->size%HASHMAP_BITMAP_TYPE_SIZE==0?0:1);
  void* mem_alloc = alloc((bitmap_size * sizeof(bitmap_t)) +
    (map->size * sizeof(hashmap_entry_t)));
  memset(mem_alloc, 0, (bitmap_size * sizeof(bitmap_t)) +
   (map->size * sizeof(hashmap_entry_t)));
  map->item_bitmap = (bitmap_t*)mem_alloc;
  map->buckets = (hashmap_entry_t*)((char *)mem_alloc +
    (bitmap_size * sizeof(bitmap_t)));

  // use hashmap scan to efficiently copy all items to new bucket array
  size_t i = HASHMAP_BEGIN;
  while((curr = ponyint_hashmap_next(&i, c, old_item_bitmap,
    s, b)) != NULL)
  {
    ponyint_hashmap_put(map, curr, b[i].hash, cmp, alloc, fr);
  }

  if((fr != NULL) && (b != NULL))
  {
    size_t old_bitmap_size = s/HASHMAP_BITMAP_TYPE_SIZE +
      (s%HASHMAP_BITMAP_TYPE_SIZE==0?0:1);
    fr((old_bitmap_size * sizeof(bitmap_t)) +
      (s * sizeof(hashmap_entry_t)), old_item_bitmap);
  }

  assert(map->count == c);
}

static size_t optimize_item(hashmap_t* map, alloc_fn alloc,
  free_size_fn fr, cmp_fn cmp, size_t old_index)
{

  size_t mask = map->size - 1;

  size_t h = map->buckets[old_index].hash;
  void* entry = map->buckets[old_index].ptr;
  size_t index = h & mask;

  for(size_t i = 0; i <= mask; i++)
  {
    // if next bucket index is current position, item is already in optimal spot
    if(index == old_index)
      break;

    // don't need to check probe counts for filled buckets because
    // earlier items are guaranteed to have a lower probe count
    // than us and we cannot displace them
    // found an earlier empty bucket so move item
    if(map->buckets[index].ptr == NULL)
    {
      ponyint_hashmap_clearindex(map, old_index);
      ponyint_hashmap_putindex(map, entry, h, cmp, alloc, fr, index);
      return 1;
    }

    // find next bucket index
    index = (index + 1) & mask;
  }

  return 0;
}

void ponyint_hashmap_init(hashmap_t* map, size_t size, alloc_fn alloc)
{
  if(size > 0)
  {
    // make sure we have room for this many elements without resizing
    size <<= 1;

    if(size < MIN_HASHMAP_SIZE)
      size = MIN_HASHMAP_SIZE;
    else
      size = ponyint_next_pow2(size);
  }

  map->count = 0;
  map->size = size;

  if(size > 0)
  {
    // use a single memory allocation to exploit spatial memory/cache locality
    size_t bitmap_size = size/HASHMAP_BITMAP_TYPE_SIZE +
      (size%HASHMAP_BITMAP_TYPE_SIZE==0?0:1);
    void* mem_alloc = alloc((bitmap_size * sizeof(bitmap_t)) +
      (size * sizeof(hashmap_entry_t)));
    memset(mem_alloc, 0, (bitmap_size * sizeof(bitmap_t)) +
      (size * sizeof(hashmap_entry_t)));
    map->item_bitmap = (bitmap_t*)mem_alloc;
    map->buckets = (hashmap_entry_t*)((char *)mem_alloc +
      (bitmap_size * sizeof(bitmap_t)));
  } else {
    map->buckets = NULL;
    map->item_bitmap = NULL;
  }
}

void ponyint_hashmap_destroy(hashmap_t* map, free_size_fn fr, free_fn free_elem)
{
  if(free_elem != NULL)
  {
    void* curr = NULL;

    // use hashmap scan to efficiently free all items
    size_t i = HASHMAP_BEGIN;
    while((curr = ponyint_hashmap_next(&i, map->count, map->item_bitmap,
      map->size, map->buckets)) != NULL)
    {
      free_elem(curr);
    }
  }

  if((fr != NULL) && (map->size > 0))
  {
    size_t bitmap_size = map->size/HASHMAP_BITMAP_TYPE_SIZE +
      (map->size%HASHMAP_BITMAP_TYPE_SIZE==0?0:1);
    fr((bitmap_size * sizeof(bitmap_t)) +
      (map->size * sizeof(hashmap_entry_t)), map->item_bitmap);
  }

  map->count = 0;
  map->size = 0;
  map->buckets = NULL;
  map->item_bitmap = NULL;
}

void* ponyint_hashmap_get(hashmap_t* map, void* key, size_t hash, cmp_fn cmp,
  size_t* pos)
{
  if(map->count == 0)
    return NULL;

  size_t probe_length = 0;
  size_t oi_probe_length = 0;
  return search(map, pos, key, hash, cmp, &probe_length, &oi_probe_length);
}

static void shift_put(hashmap_t* map, void* entry, size_t hash, cmp_fn cmp,
  alloc_fn alloc, free_size_fn fr, size_t index, size_t pl, size_t oi_pl,
  void *e)
{
  void* elem = e;

  size_t ci_hash = hash;
  void* ci_entry = entry;
  size_t oi_hash = 0;
  void* oi_entry = NULL;
  size_t pos = index;
  size_t probe_length = pl;
  size_t oi_probe_length = oi_pl;

  assert(probe_length > oi_probe_length ||
    (probe_length == oi_probe_length && probe_length == 0));

  while(true) {
    assert(pos < map->size);

    // need to swap elements
    if(elem == NULL && map->buckets[pos].ptr != NULL)
    {
      // save old element info
      oi_entry = map->buckets[pos].ptr;
      oi_hash = map->buckets[pos].hash;

      // put new element
      map->buckets[pos].ptr = ci_entry;
      map->buckets[pos].hash = ci_hash;

      // set currenty entry we're putting to be old entry
      ci_entry = oi_entry;
      ci_hash = oi_hash;

      // get old entry probe count
      probe_length = oi_probe_length;

      // find next item
      elem = search(map, &pos, ci_entry, ci_hash, cmp, &probe_length,
        &oi_probe_length);

      // keep going
      continue;
    }

    // put item into bucket
    map->buckets[pos].ptr = ci_entry;
    map->buckets[pos].hash = ci_hash;

    // we put a new item in an empty bucket
    if(elem == NULL)
    {
      map->count++;

      // update item bitmap
      size_t ib_index = pos/HASHMAP_BITMAP_TYPE_SIZE;
      size_t ib_offset = pos%HASHMAP_BITMAP_TYPE_SIZE;
      map->item_bitmap[ib_index] |= ((bitmap_t)1 << ib_offset);

      if((map->count << 1) > map->size)
        resize(map, cmp, alloc, fr);
    }

    return;
  }
}

void* ponyint_hashmap_put(hashmap_t* map, void* entry, size_t hash, cmp_fn cmp,
  alloc_fn alloc, free_size_fn fr)
{
  if(map->size == 0)
    ponyint_hashmap_init(map, 4, alloc);

  size_t pos;
  size_t probe_length = 0;
  size_t oi_probe_length = 0;

  void* elem = search(map, &pos, entry, hash, cmp, &probe_length,
    &oi_probe_length);

  shift_put(map, entry, hash, cmp, alloc, fr, pos, probe_length,
    oi_probe_length, elem);

  return elem;
}

void ponyint_hashmap_putindex(hashmap_t* map, void* entry, size_t hash,
  cmp_fn cmp, alloc_fn alloc, free_size_fn fr, size_t pos)
{
  if(pos == HASHMAP_UNKNOWN)
  {
    ponyint_hashmap_put(map, entry, hash, cmp, alloc, fr);
    return;
  }

  if(map->size == 0)
    ponyint_hashmap_init(map, 4, alloc);

  assert(pos < map->size);

  // if bucket is empty
  if(map->buckets[pos].ptr == NULL)
  {
    map->buckets[pos].ptr = entry;
    map->buckets[pos].hash = hash;
    map->count++;

    // update item bitmap
    size_t ib_index = pos/HASHMAP_BITMAP_TYPE_SIZE;
    size_t ib_offset = pos%HASHMAP_BITMAP_TYPE_SIZE;
    map->item_bitmap[ib_index] |= ((bitmap_t)1 << ib_offset);


    if((map->count << 1) > map->size)
      resize(map, cmp, alloc, fr);
  } else {
    size_t mask = map->size - 1;

    // save old item info
    size_t oi_hash = map->buckets[pos].hash;
    size_t oi_probe_length = get_probe_length(map, oi_hash, pos, mask);

    size_t ci_probe_length = get_probe_length(map, hash, pos, mask);

    // if item to be put has a greater probe length
    if(ci_probe_length > oi_probe_length)
    {
      // use shift_put to bump existing item
      shift_put(map, entry, hash, cmp, alloc, fr, pos, ci_probe_length,
        oi_probe_length, NULL);
    } else {
      // we would break our smallest probe length wins guarantee
      // and so cannot bump existing element and need to put
      // new item via normal put operation
      ponyint_hashmap_put(map, entry, hash, cmp, alloc, fr);
    }
  }
}

static void shift_delete(hashmap_t* map, size_t index)
{
  assert(index < map->size);
  size_t pos = index;
  size_t mask = map->size - 1;
  size_t next_pos = (pos + 1) & mask;
  void* ni_elem = map->buckets[next_pos].ptr;
  size_t ni_hash = map->buckets[next_pos].hash;

  while(ni_elem != NULL && get_probe_length(map, ni_hash, next_pos, mask) != 0)
  {
    // shift item back into now empty bucket
    map->buckets[pos].ptr = map->buckets[next_pos].ptr;
    map->buckets[pos].hash = map->buckets[next_pos].hash;

    // increment position
    pos = next_pos;
    next_pos = (pos + 1) & mask;

    // get next item info
    ni_elem = map->buckets[next_pos].ptr;
    ni_hash = map->buckets[next_pos].hash;
  }

  // done shifting all required elements; set current position as empty
  // and decrement count
  map->buckets[pos].ptr = NULL;
  map->count--;

  // update item bitmap
  size_t ib_index = pos/HASHMAP_BITMAP_TYPE_SIZE;
  size_t ib_offset = pos%HASHMAP_BITMAP_TYPE_SIZE;
  map->item_bitmap[ib_index] &= ~((bitmap_t)1 << ib_offset);
}

void* ponyint_hashmap_remove(hashmap_t* map, void* entry, size_t hash,
  cmp_fn cmp)
{
  if(map->count == 0)
    return NULL;

  size_t pos;
  size_t probe_length = 0;
  size_t oi_probe_length = 0;
  void* elem = search(map, &pos, entry, hash, cmp, &probe_length,
    &oi_probe_length);

  if(elem != NULL)
    shift_delete(map, pos);

  return elem;
}

void ponyint_hashmap_removeindex(hashmap_t* map, size_t index)
{
  if(map->size <= index)
    return;

  void* elem = map->buckets[index].ptr;

  if(elem != NULL)
    shift_delete(map, index);
}

void* ponyint_hashmap_next(size_t* i, size_t count, bitmap_t* item_bitmap,
  size_t size, hashmap_entry_t* buckets)
{
  if(count == 0)
    return NULL;

  size_t index = *i + 1;
  size_t ib_index = index/HASHMAP_BITMAP_TYPE_SIZE;
  size_t ib_offset = index%HASHMAP_BITMAP_TYPE_SIZE;
  size_t ffs_offset = 0;

  // get bitmap entry
  // right shift to get rid of old 1 bits we don't care about
  bitmap_t ib = item_bitmap[ib_index] >> ib_offset;

  while(index < size)
  {
    // find first set bit using ffs
    ffs_offset = __pony_ffsl(ib);

    // if no bits set; increment index to next item bitmap entry
    if(ffs_offset == 0)
    {
      index += (HASHMAP_BITMAP_TYPE_SIZE - ib_offset);
      ib_index++;
      ib_offset = 0;
      ib = item_bitmap[ib_index];
      continue;
    } else {
      // found a set bit for valid element
      index += (ffs_offset - 1);

      // no need to check if valid element because item bitmap keeps track of it
      assert(buckets[index].ptr != NULL);
      *i = index;
      return buckets[index].ptr;
    }
  }

  // searched through bitmap and didn't find any more valid elements.
  // index could be bigger than size due to use of ffs
  *i = size;
  return NULL;
}

size_t ponyint_hashmap_size(hashmap_t* map)
{
  return map->count;
}

void ponyint_hashmap_clearindex(hashmap_t* map, size_t index)
{
  if(map->size <= index)
    return;

  void* elem = map->buckets[index].ptr;

  if(elem == NULL)
    return;

  map->buckets[index].ptr = NULL;
  map->count--;

  // update item bitmap
  size_t ib_index = index/HASHMAP_BITMAP_TYPE_SIZE;
  size_t ib_offset = index%HASHMAP_BITMAP_TYPE_SIZE;
  map->item_bitmap[ib_index] &= ~((bitmap_t)1 << ib_offset);
}

void ponyint_hashmap_optimize(hashmap_t* map, alloc_fn alloc,
  free_size_fn fr, cmp_fn cmp)
{
  size_t count = 0;
  size_t num_iters = 0;
  size_t i = HASHMAP_BEGIN;
  void* elem;

  do
  {
    count = 0;
    i = HASHMAP_BEGIN;
    while((elem = ponyint_hashmap_next(&i, map->count, map->item_bitmap,
      map->size, map->buckets)) != NULL)
    {
      count += optimize_item(map, alloc, fr, cmp, i);
    }
    num_iters++;
  } while(count > 0);
}

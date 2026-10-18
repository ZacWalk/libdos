/* libdos by Zac Walker
 *
 * The support library described in support.h. Nothing here knows anything
 * about C, tokens or code generation; it is the memory model the rest of the
 * compiler is written against.
 */

#include "support.h"

#include <stdlib.h>
#include <string.h>

/* ---- arena ---- */

#define CC_ARENA_ALIGN 8

static size_t cc_round_up(size_t value, size_t to)
{
	return (value + to - 1) / to * to;
}

void cc_arena_init(cc_arena* a, size_t block_size)
{
	a->block = NULL;
	a->block_size = block_size != 0 ? block_size : (size_t)256 * 1024;
	a->total = 0;
	a->exhausted = 0;
}

void cc_arena_free(cc_arena* a)
{
	cc_arena_block* block = a->block;
	while (block != NULL)
	{
		cc_arena_block* previous = block->previous;
		free(block);
		block = previous;
	}
	a->block = NULL;
	a->total = 0;
	a->exhausted = 0;
}

void* cc_arena_alloc(cc_arena* a, size_t bytes)
{
	size_t need;
	size_t header;
	char* at;

	need = cc_round_up(bytes != 0 ? bytes : 1, CC_ARENA_ALIGN);
	header = cc_round_up(sizeof(cc_arena_block), CC_ARENA_ALIGN);

	if (a->block == NULL || a->block->used + need > a->block->size)
	{
		/* A block big enough for this on its own, so one large request does not
		 * strand the rest of the block it did not fit in. */
		size_t size = a->block_size;
		cc_arena_block* block;
		if (need + header > size) size = need + header;
		block = (cc_arena_block*)malloc(size);
		if (block == NULL)
		{
			a->exhausted = 1;
			return NULL;
		}
		block->previous = a->block;
		block->size = size;
		block->used = header;
		a->block = block;
	}

	at = (char*)a->block + a->block->used;
	a->block->used += need;
	a->total += need;
	memset(at, 0, need);
	return at;
}

char* cc_arena_copy(cc_arena* a, const char* text, size_t length)
{
	char* copy = (char*)cc_arena_alloc(a, length + 1);
	if (copy == NULL) return NULL;
	if (length != 0) memcpy(copy, text, length);
	copy[length] = 0;
	return copy;
}

/* ---- growable array ---- */

void cc_list_init(cc_list* l, int item_size)
{
	l->items = NULL;
	l->count = 0;
	l->capacity = 0;
	l->item_size = item_size;
}

void cc_list_free(cc_list* l)
{
	free(l->items);
	l->items = NULL;
	l->count = 0;
	l->capacity = 0;
}

static int cc_list_reserve(cc_list* l, int capacity)
{
	int wanted;
	void* items;

	if (capacity <= l->capacity) return 1;

	wanted = l->capacity != 0 ? l->capacity * 2 : 16;
	if (wanted < capacity) wanted = capacity;

	items = realloc(l->items, (size_t)wanted * (size_t)l->item_size);
	if (items == NULL) return 0;

	l->items = items;
	l->capacity = wanted;
	return 1;
}

void* cc_list_add(cc_list* l)
{
	char* at;
	if (!cc_list_reserve(l, l->count + 1)) return NULL;
	at = (char*)l->items + (size_t)l->count * (size_t)l->item_size;
	memset(at, 0, (size_t)l->item_size);
	l->count += 1;
	return at;
}

void* cc_list_at(const cc_list* l, int index)
{
	return (char*)l->items + (size_t)index * (size_t)l->item_size;
}

int cc_list_set_count(cc_list* l, int count)
{
	if (count > l->count)
	{
		if (!cc_list_reserve(l, count)) return 0;
		memset((char*)l->items + (size_t)l->count * (size_t)l->item_size, 0,
		       (size_t)(count - l->count) * (size_t)l->item_size);
	}
	l->count = count;
	return 1;
}

/* ---- sorted table ---- */

void cc_map_init(cc_map* m)
{
	cc_list_init(&m->pairs, (int)sizeof(cc_pair));
}

void cc_map_free(cc_map* m)
{
	cc_list_free(&m->pairs);
}

int cc_map_lower_bound(const cc_map* m, int key)
{
	const cc_pair* pairs = (const cc_pair*)m->pairs.items;
	int low = 0;
	int high = m->pairs.count;

	while (low < high)
	{
		int middle = low + (high - low) / 2;
		if (pairs[middle].key < key) low = middle + 1;
		else high = middle;
	}
	return low;
}

int cc_map_find(const cc_map* m, int key, int* value)
{
	int at = cc_map_lower_bound(m, key);
	const cc_pair* pairs = (const cc_pair*)m->pairs.items;
	if (at >= m->pairs.count || pairs[at].key != key) return 0;
	if (value != NULL) *value = pairs[at].value;
	return 1;
}

int cc_map_get(const cc_map* m, int key, int missing)
{
	int value = missing;
	if (!cc_map_find(m, key, &value)) return missing;
	return value;
}

void cc_map_set(cc_map* m, int key, int value)
{
	int at = cc_map_lower_bound(m, key);
	cc_pair* pairs;

	pairs = (cc_pair*)m->pairs.items;
	if (at < m->pairs.count && pairs[at].key == key)
	{
		pairs[at].value = value;
		return;
	}

	if (cc_list_add(&m->pairs) == NULL) return;
	pairs = (cc_pair*)m->pairs.items;
	if (at < m->pairs.count - 1)
		memmove(&pairs[at + 1], &pairs[at], (size_t)(m->pairs.count - 1 - at) * sizeof(cc_pair));
	pairs[at].key = key;
	pairs[at].value = value;
}

int cc_map_count(const cc_map* m)
{
	return m->pairs.count;
}

cc_pair* cc_map_pair(const cc_map* m, int index)
{
	return (cc_pair*)cc_list_at(&m->pairs, index);
}

/* ---- interned names ---- */

static unsigned cc_hash(const char* text, int length)
{
	unsigned hash = 2166136261u;
	int i;
	for (i = 0; i < length; ++i)
	{
		hash ^= (unsigned char)text[i];
		hash *= 16777619u;
	}
	return hash;
}

void cc_names_init(cc_names* n, cc_arena* a)
{
	n->arena = a;
	cc_list_init(&n->entries, (int)sizeof(char*));
	n->buckets = NULL;
	n->bucket_count = 0;
}

void cc_names_free(cc_names* n)
{
	cc_list_free(&n->entries);
	free(n->buckets);
	n->buckets = NULL;
	n->bucket_count = 0;
}

/* Open addressing: a bucket holds an id, and zero means empty. */
static int cc_names_rehash(cc_names* n, int bucket_count)
{
	int* buckets = (int*)calloc((size_t)bucket_count, sizeof(int));
	int i;

	if (buckets == NULL) return 0;

	for (i = 0; i < n->entries.count; ++i)
	{
		const char* text = CC_AT(n->entries, char*, i);
		unsigned at = cc_hash(text, (int)strlen(text)) & (unsigned)(bucket_count - 1);
		while (buckets[at] != 0) at = (at + 1) & (unsigned)(bucket_count - 1);
		buckets[at] = i + 1;
	}

	free(n->buckets);
	n->buckets = buckets;
	n->bucket_count = bucket_count;
	return 1;
}

int cc_intern(cc_names* n, const char* text, int length)
{
	unsigned at;
	char** slot;
	char* copy;

	if (text == NULL) return 0;
	if (length < 0) length = (int)strlen(text);

	if (n->bucket_count == 0 && !cc_names_rehash(n, 256)) return 0;
	if ((n->entries.count + 1) * 10 >= n->bucket_count * 7 &&
	    !cc_names_rehash(n, n->bucket_count * 2))
		return 0;

	at = cc_hash(text, length) & (unsigned)(n->bucket_count - 1);
	while (n->buckets[at] != 0)
	{
		const char* found = CC_AT(n->entries, char*, n->buckets[at] - 1);
		if (strncmp(found, text, (size_t)length) == 0 && found[length] == 0)
			return n->buckets[at];
		at = (at + 1) & (unsigned)(n->bucket_count - 1);
	}

	copy = cc_arena_copy(n->arena, text, (size_t)length);
	if (copy == NULL) return 0;
	slot = (char**)cc_list_add(&n->entries);
	if (slot == NULL) return 0;
	*slot = copy;

	n->buckets[at] = n->entries.count;
	return n->buckets[at];
}

const char* cc_name_text(const cc_names* n, int id)
{
	if (id <= 0 || id > n->entries.count) return "";
	return CC_AT(n->entries, char*, id - 1);
}

int cc_names_count(const cc_names* n)
{
	return n->entries.count;
}

/* ---- source text ---- */

static size_t cc_write_utf8(char* out, unsigned code)
{
	if (code < 0x80)
	{
		out[0] = (char)code;
		return 1;
	}
	if (code < 0x800)
	{
		out[0] = (char)(0xc0 | (code >> 6));
		out[1] = (char)(0x80 | (code & 0x3f));
		return 2;
	}
	if (code < 0x10000)
	{
		out[0] = (char)(0xe0 | (code >> 12));
		out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
		out[2] = (char)(0x80 | (code & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (code >> 18));
	out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
	out[3] = (char)(0x80 | (code & 0x3f));
	return 4;
}

char* cc_normalise_source(cc_arena* a, const void* bytes, size_t size, size_t* length)
{
	const unsigned char* in = (const unsigned char*)bytes;
	char* out;
	size_t written = 0;
	size_t i;
	int wide = 0; /* 1 little endian, 2 big endian */

	if (size >= 2 && in[0] == 0xff && in[1] == 0xfe) { wide = 1; in += 2; size -= 2; }
	else if (size >= 2 && in[0] == 0xfe && in[1] == 0xff) { wide = 2; in += 2; size -= 2; }
	else if (size >= 3 && in[0] == 0xef && in[1] == 0xbb && in[2] == 0xbf) { in += 3; size -= 3; }

	/* Three bytes per UTF-16 unit is the worst case, and a surrogate pair is
	 * two units for four bytes, so this covers both. */
	out = (char*)cc_arena_alloc(a, (wide != 0 ? size / 2 * 3 : size) + 1);
	if (out == NULL) return NULL;

	if (wide != 0)
	{
		for (i = 0; i + 1 < size; i += 2)
		{
			unsigned unit = wide == 1 ? (unsigned)(in[i] | (in[i + 1] << 8))
			                          : (unsigned)((in[i] << 8) | in[i + 1]);
			if (unit >= 0xd800 && unit < 0xdc00 && i + 3 < size)
			{
				unsigned low = wide == 1 ? (unsigned)(in[i + 2] | (in[i + 3] << 8))
				                         : (unsigned)((in[i + 2] << 8) | in[i + 3]);
				if (low >= 0xdc00 && low < 0xe000)
				{
					unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
					i += 2;
				}
			}
			if (unit >= 0xd800 && unit < 0xe000) unit = 0xfffd; /* an unpaired half */
			written += cc_write_utf8(out + written, unit);
		}
	}
	else
	{
		if (size != 0) memcpy(out, in, size);
		written = size;
	}

	/* Line endings and the DOS end-of-file marker, in place: the result is
	 * never longer than what it started as. */
	{
		size_t read = 0;
		size_t put = 0;
		while (read < written)
		{
			char c = out[read];
			if (c == 0x1a) break;
			if (c == '\r')
			{
				if (read + 1 < written && out[read + 1] == '\n') ++read;
				c = '\n';
			}
			out[put++] = c;
			++read;
		}
		written = put;
	}

	out[written] = 0;
	if (length != NULL) *length = written;
	return out;
}

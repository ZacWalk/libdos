#ifndef OVRWIN_CC_SUPPORT_H
#define OVRWIN_CC_SUPPORT_H

/* ovrwin by Zac Walker
 *
 * The four things the compiler needs from a standard library once it is C: a
 * bump allocator, a growable array, a sorted table of small integers, and a
 * place to keep every identifier exactly once.
 *
 * The compiler is one process compiling one program and then stopping, so
 * nothing here frees an individual object. Memory is handed out in blocks and
 * given back in blocks, which removes the ownership question from every other
 * file - and removes the reason the C++ version needed std::string at all.
 *
 * This header is meant to be read by cc itself one day, so: include guards
 * rather than #pragma once, no // comments in the interface, and nothing
 * returned by value that is bigger than a pointer.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- arena ---- */

typedef struct cc_arena_block cc_arena_block;

struct cc_arena_block
{
	cc_arena_block* previous;
	size_t size;
	size_t used;
};

typedef struct cc_arena
{
	cc_arena_block* block;
	size_t block_size;
	size_t total; /* every byte asked for, for reporting */
	int exhausted; /* an allocation failed; nothing here is trustworthy after that */
} cc_arena;

void cc_arena_init(cc_arena* a, size_t block_size);
void cc_arena_free(cc_arena* a);

/* Eight-byte aligned and zeroed. NULL when the host is out of memory, which
 * also latches `exhausted` so a caller can check once rather than every time. */
void* cc_arena_alloc(cc_arena* a, size_t bytes);

/* A NUL-terminated copy, which is how a name or a file's text is kept. */
char* cc_arena_copy(cc_arena* a, const char* text, size_t length);

/* ---- growable array ---- */

/* A bump allocator cannot grow its last block once something follows it, so
 * the handful of collections that genuinely grow use realloc instead. */
typedef struct cc_list
{
	void* items;
	int count;
	int capacity;
	int item_size;
} cc_list;

void cc_list_init(cc_list* l, int item_size);
void cc_list_free(cc_list* l);

/* A new zeroed slot at the end, or NULL if it could not grow. */
void* cc_list_add(cc_list* l);

void* cc_list_at(const cc_list* l, int index);

/* Truncates, or extends with zeroed slots. This is what speculation rewinds. */
int cc_list_set_count(cc_list* l, int count);

#define CC_AT(list, type, index) (((type*)(list).items)[index])

/* ---- sorted table ---- */

typedef struct cc_pair
{
	int key;
	int value;
} cc_pair;

/* Kept in key order, so a lookup is a binary search and the order something is
 * walked in does not depend on where its name landed in a hash table. */
typedef struct cc_map
{
	cc_list pairs;
} cc_map;

void cc_map_init(cc_map* m);
void cc_map_free(cc_map* m);

/* The first entry whose key is not less than `key`. Equal to count when there
 * is none, which is also where an insert of `key` would go. */
int cc_map_lower_bound(const cc_map* m, int key);

/* Non-zero when the key is there, with the value written through `value`. */
int cc_map_find(const cc_map* m, int key, int* value);

int cc_map_get(const cc_map* m, int key, int missing);
void cc_map_set(cc_map* m, int key, int value);

int cc_map_count(const cc_map* m);
cc_pair* cc_map_pair(const cc_map* m, int index);

/* ---- interned names ---- */

/* An identifier is an integer from here on. Zero is no name at all. */
typedef struct cc_names
{
	cc_arena* arena;
	cc_list entries; /* char*, indexed by id - 1 */
	int* buckets;
	int bucket_count;
} cc_names;

void cc_names_init(cc_names* n, cc_arena* a);
void cc_names_free(cc_names* n);

/* `length` of -1 means the text is NUL-terminated. */
int cc_intern(cc_names* n, const char* text, int length);

const char* cc_name_text(const cc_names* n, int id);
int cc_names_count(const cc_names* n);

/* ---- source text ---- */

/* One text encoding reaches the scanner, whatever the file was: UTF-16 with a
 * byte order mark becomes UTF-8, a UTF-8 mark is dropped, CRLF and a lone CR
 * become LF, and the text stops at the first Ctrl+Z - old DOS sources carry
 * one and every compiler that has ever read them treats it as end of file.
 *
 * The result lives in the arena, NUL-terminated, and stays there for the whole
 * compile: a token can then be a pointer into it rather than a copy of it. */
char* cc_normalise_source(cc_arena* a, const void* bytes, size_t size, size_t* length);

#ifdef __cplusplus
}
#endif

#endif

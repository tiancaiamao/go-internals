// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

struct Hmap;		/* opaque */

/* 由垃圾回收器使用 */
struct hash_gciter
{
	Hmap *h;
	int32 phase;
	uintptr bucket;
	struct Bucket *b;
	uintptr i;
};

// this data is used by the garbage collector to keep the map's
// internal structures from being reclaimed.  The iterator must
// return in st every live object (ones returned by mallocgc) so
// that those objects won't be collected, and it must return
// every key & value in key_data/val_data so they can get scanned
// for pointers they point to.  Note that if you malloc storage
// for keys and values, you need to do both.
// 这个数据是由垃圾回收器使用的，为了防止map的内部结构被当作垃圾被回收
// st是由mallocgc返回的map的内部结构对象，kay_data和val_data分别是key/value对象的指针。
// 这里解释一下，map中的一个数据，无论是key或者value，只要是没有指针能引用到它了，垃圾回收时的标记就无法标记，在清扫阶段就会被当作垃圾回收
// 所以，为了保证活着的对象不被当垃圾回收掉，map的内部实现上会用一个指针引用到它们
struct hash_gciter_data
{
	uint8 *st;			/* internal structure, or nil */
	uint8 *key_data;		/* key data, or nil */
	uint8 *val_data;		/* value data, or nil */
	bool indirectkey;		/* storing pointers to keys */
	bool indirectval;		/* storing pointers to values */
};
bool hash_gciter_init (struct Hmap *h, struct hash_gciter *it);
bool hash_gciter_next (struct hash_gciter *it, struct hash_gciter_data *data);

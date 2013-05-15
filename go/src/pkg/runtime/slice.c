// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"
#include "arch_GOARCH.h"
#include "type.h"
#include "typekind.h"
#include "malloc.h"
#include "race.h"

static	bool	debug	= 0;

static	void	makeslice1(SliceType*, intgo, intgo, Slice*);
static	void	growslice1(SliceType*, Slice, intgo, Slice *);
	void	runtime·copy(Slice to, Slice fm, uintptr width, intgo ret);

// make([]T, len, cap)会调用到这个函数来。注意这里的ret是整个结构体传进来的。所以按go的函数调用协议，返回值在栈中的布局其实是一个结构体而不是结构体指针。
// see also unsafe·NewArray
// makeslice(typ *Type, len, cap int64) (ary []any);
void
runtime·makeslice(SliceType *t, int64 len, int64 cap, Slice ret)
{
	// NOTE: The len > MaxMem/elemsize check here is not strictly necessary,
	// but it produces a 'len out of range' error instead of a 'cap out of range' error
	// when someone does make([]T, bignumber). 'cap out of range' is true too,
	// but since the cap is only being supplied implicitly, saying len is clearer.
	// See issue 4085.
	//(intgo)len != len是将len强制转换为go的int(依赖于机器位数,32或64),这就说明make([]T, len, cap)上面支持的len其实是int类型的
	//也就意味着，在32位机器上，传一个很大的数，超过32位能表示的范围是会出错的。64位机器上没什么感觉。
	if(len < 0 || (intgo)len != len || t->elem->size > 0 && len > MaxMem / t->elem->size)
		runtime·panicstring("makeslice: len out of range");

	if(cap < len || (intgo)cap != cap || t->elem->size > 0 && cap > MaxMem / t->elem->size)
		runtime·panicstring("makeslice: cap out of range");

	makeslice1(t, len, cap, &ret);

	if(debug) {
		runtime·printf("makeslice(%S, %D, %D); ret=",
			*t->string, len, cap);
		runtime·printslice(ret);
	}
}

// Dummy word to use as base pointer for make([]T, 0).
// Since you cannot take the address of such a slice,
// you can't tell that they all have the same base pointer.
//优化，由于make([]T, 0)不必要真正分配空间， 但是slice->array最好不要是空值
uintptr runtime·zerobase;

static void
makeslice1(SliceType *t, intgo len, intgo cap, Slice *ret)
{
	uintptr size;

	size = cap*t->elem->size;

	ret->len = len;
	ret->cap = cap;

	if(size == 0)
		ret->array = (byte*)&runtime·zerobase;
	else if((t->elem->kind&KindNoPointers)) //elem是一个Type结构，记录了类型信息，FlagNoPointers信息对垃圾回收很有用
		ret->array = runtime·mallocgc(size, FlagNoPointers, 1, 1);
	else {
		ret->array = runtime·mallocgc(size, 0, 1, 1);

		if(UseSpanType) { //UseSpanType是enum的1  这里恒为真
			if(false) {
				runtime·printf("new slice [%D]%S: %p\n", (int64)cap, *t->elem->string, ret->array);
			}
			runtime·settype(ret->array, (uintptr)t->elem | TypeInfo_Array); //也是为精确的垃圾回收提供信息。有类型信息后垃圾回收可以做得更精确
		}
	}
}

// appendslice(type *Type, x, y, []T) []T
#pragma textflag 7
void
runtime·appendslice(SliceType *t, Slice x, Slice y, Slice ret)
{
	intgo m;
	uintptr w;
	void *pc;
	uint8 *p, *q;

	m = x.len+y.len;
	w = t->elem->size;

	if(m < x.len)
		runtime·throw("append: slice overflow");

	//如果前一个slice的cap是足够大的，则直接将后一个赋值过来，否则要进行grow
	if(m > x.cap)
		growslice1(t, x, m, &ret);
	else
		ret = x;

	if(raceenabled) {
		// Don't mark read/writes on the newly allocated slice.
		pc = runtime·getcallerpc(&t);
		// read x[:len]
		if(m > x.cap)
			runtime·racereadrangepc(x.array, x.len*w, w, pc, runtime·appendslice);
		// read y
		runtime·racereadrangepc(y.array, y.len*w, w, pc, runtime·appendslice);
		// write x[len(x):len(x)+len(y)]
		if(m <= x.cap)
			runtime·racewriterangepc(ret.array+ret.len*w, y.len*w, w, pc, runtime·appendslice);
	}

	// A very common case is appending bytes. Small appends can avoid the overhead of memmove.
	// We can generalize a bit here, and just pick small-sized appends.
	//如果只是复制很少的内容，直接for循环赋值效率会高一些，避免了memmove函数调用的开销
	p = ret.array+ret.len*w;
	q = y.array;
	w *= y.len;
	if(w <= appendCrossover) { //appendCrossover在386和amd64中是16，在arm中是8
		if(p <= q || w <= p-q) // No overlap.
			while(w-- > 0)
				*p++ = *q++;
		else {
			p += w;
			q += w;
			while(w-- > 0)
				*--p = *--q;
		}
	} else {
		runtime·memmove(p, q, w);
	}
	ret.len += y.len;
	FLUSH(&ret); //go的多值返回在c这边的实现方式
}


// appendstr([]byte, string) []byte
#pragma textflag 7
void
runtime·appendstr(SliceType *t, Slice x, String y, Slice ret)
{
	intgo m;
	void *pc;
	uintptr w;
	uint8 *p, *q;

	m = x.len+y.len;

	if(m < x.len)
		runtime·throw("append: string overflow");

	if(m > x.cap)
		growslice1(t, x, m, &ret);
	else
		ret = x;

	if(raceenabled) {
		// Don't mark read/writes on the newly allocated slice.
		pc = runtime·getcallerpc(&t);
		// read x[:len]
		if(m > x.cap)
			runtime·racereadrangepc(x.array, x.len, 1, pc, runtime·appendstr);
		// write x[len(x):len(x)+len(y)]
		if(m <= x.cap)
			runtime·racewriterangepc(ret.array+ret.len, y.len, 1, pc, runtime·appendstr);
	}

	// Small appends can avoid the overhead of memmove.
	w = y.len;
	p = ret.array+ret.len;
	q = y.str;
	if(w <= appendCrossover) {
		while(w-- > 0)
			*p++ = *q++;
	} else {
		runtime·memmove(p, q, w);
	}
	ret.len += y.len;
	FLUSH(&ret);
}

// growslice(type *Type, x, []T, n int64) []T
void
runtime·growslice(SliceType *t, Slice old, int64 n, Slice ret)
{
	int64 cap;
	void *pc;

	if(n < 1)
		runtime·panicstring("growslice: invalid n");

	cap = old.cap + n;

	if((intgo)cap != cap || cap < old.cap || (t->elem->size > 0 && cap > MaxMem/t->elem->size))
		runtime·panicstring("growslice: cap out of range");

	if(raceenabled) {
		pc = runtime·getcallerpc(&t);
		runtime·racereadrangepc(old.array, old.len*t->elem->size, t->elem->size, pc, runtime·growslice);
	}

	growslice1(t, old, cap, &ret);

	FLUSH(&ret);

	if(debug) {
		runtime·printf("growslice(%S,", *t->string);
		runtime·printslice(old);
		runtime·printf(", new cap=%D) =", cap);
		runtime·printslice(ret);
	}
}

//grows的大小增长规则：
//如果新的大小是当前大小2倍以上，则大小增长为新大小
//否则，如果当前大小小于1024个，按每次2倍增长。
//否则，按每次是上一次大小的1.25倍增长
static void
growslice1(SliceType *t, Slice x, intgo newcap, Slice *ret)
{
	intgo m;

	m = x.cap;
	
	// Using newcap directly for m+m < newcap handles
	// both the case where m == 0 and also the case where
	// m+m/4 wraps around, in which case the loop
	// below might never terminate.
	if(m+m < newcap)
		m = newcap;
	else {
		do {
			if(x.len < 1024)
				m += m;
			else
				m += m/4;
		} while(m < newcap);
	}
	makeslice1(t, x.len, m, ret);
	runtime·memmove(ret->array, x.array, ret->len * t->elem->size);
}

// copy(to any, fr any, wid uintptr) int
#pragma textflag 7
void
runtime·copy(Slice to, Slice fm, uintptr width, intgo ret)
{
	void *pc;

	if(fm.len == 0 || to.len == 0 || width == 0) {
		ret = 0;
		goto out;
	}

	ret = fm.len;
	if(to.len < ret)
		ret = to.len;

	if(raceenabled) {
		pc = runtime·getcallerpc(&to);
		runtime·racewriterangepc(to.array, ret*width, width, pc, runtime·copy);
		runtime·racereadrangepc(fm.array, ret*width, width, pc, runtime·copy);
	}

	if(ret == 1 && width == 1) {	// common case worth about 2x to do here
		*to.array = *fm.array;	// known to be a byte pointer
	} else {
		runtime·memmove(to.array, fm.array, ret*width);
	}

out:
	FLUSH(&ret);

	if(debug) {
		runtime·prints("main·copy: to=");
		runtime·printslice(to);
		runtime·prints("; fm=");
		runtime·printslice(fm);
		runtime·prints("; width=");
		runtime·printint(width);
		runtime·prints("; ret=");
		runtime·printint(ret);
		runtime·prints("\n");
	}
}

#pragma textflag 7
void
runtime·slicestringcopy(Slice to, String fm, intgo ret)
{
	void *pc;

	if(fm.len == 0 || to.len == 0) {
		ret = 0;
		goto out;
	}

	ret = fm.len;
	if(to.len < ret)
		ret = to.len;

	if(raceenabled) {
		pc = runtime·getcallerpc(&to);
		runtime·racewriterangepc(to.array, ret, 1, pc, runtime·slicestringcopy);
	}

	runtime·memmove(to.array, fm.str, ret);

out:
	FLUSH(&ret);
}

void
runtime·printslice(Slice a)
{
	runtime·prints("[");
	runtime·printint(a.len);
	runtime·prints("/");
	runtime·printint(a.cap);
	runtime·prints("]");
	runtime·printpointer(a.array);
}

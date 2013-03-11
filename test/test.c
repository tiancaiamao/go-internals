#include "runtime.h"
#include "malloc.h"
#include "type.h"

static void mtypeinfo(MTypes *mt)
{
	uintptr ptr;
	Type* t;
	for(int32 i=1; i<8; i++) {
		ptr = ((uintptr*)mt->data)[i];
		if(ptr == 0)
			continue;
		t = (Type*)(ptr & ~(uintptr)(PtrSize-1));
		switch(ptr & (PtrSize-1)) {
		case TypeInfo_SingleObject:
			runtime·printf("SingleObject");
			break;
		case TypeInfo_Array:
			runtime·printf("Array");
			break;
		case TypeInfo_Map:
			runtime·printf("Map");
			break;
		case TypeInfo_Chan:
			runtime·printf("Chan");
			break;
		default:
			runtime·throw("error: not right typeinfo");
		}
		runtime·printf("第%d类的类型信息，大小:%D,类型:%S\n", i, t->size, *t->string);
	}
}

static void mspaninfo(MSpan *s)
{
	runtime·printf("---------------\n");
	runtime·printf("页号:%D,页数:%D,大小类:%d，元素大小:%D\n", s->start, s->npages, s->sizeclass, s->elemsize);

	switch(s->types.compression) {
	case MTypes_Empty:
		break;
	case MTypes_Single:
		runtime·printf("MTypes_Single\n");
		break;
	case MTypes_Words:
		runtime·printf("MTypes_Words\n");
		break;
	case MTypes_Bytes:
		runtime·printf("MTypes_Bytes\n");
		
		mtypeinfo(&s->types);
		break;
	default:
		runtime·throw("error: unkonwn type in MSpans");
	}
}

void ·MemInfo()
{
	MSpan *s;
	MHeap *h;
	uint32 i;

	h = runtime·mheap;
	runtime·printf("使用的堆的起始地址：%p,结束地址:%p,已使用:%p\n", h->arena_start,h->arena_end,h->arena_used);
	runtime·printf("使用中的MSpan的信息:\n");
	for(i=0; i < h->nspan; i++) {
		s = h->allspans[i];
		if(s == nil || s->state != MSpanInUse)
			continue;
		mspaninfo(s);
	}
}

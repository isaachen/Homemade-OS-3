#include"common.h"
#include"kernel.h"
#include"memory.h"
#include"memory_private.h"
#include"multiprocessor/spinlock.h"
#include"assembly/assembly.h"

typedef union MemoryUnit{
	union MemoryUnit *next;
	int address[0];
}MemoryUnit;

static_assert(sizeof(MemoryUnit) == 4);

// SLAB_SIZE must be multiple of MIN_BLOCK_SIZE
#define SLAB_SIZE (MIN_BLOCK_SIZE)

typedef struct Slab{
	struct Slab *next, **prev;
	size_t usedCount;
	MemoryUnit *freeList;
}Slab;

static int isTotallyFree(Slab *p){
	return p->usedCount == 0;
}

static int isTotallyUsed(Slab *p){
	return p->freeList == NULL;
}

static void initSlab(Slab *slab, size_t unit){
	slab->prev = NULL;
	slab->next = NULL;
	slab->usedCount = 0;
	uintptr_t p = ((uintptr_t)slab);
	p += sizeof(Slab);
	MemoryUnit *fl = NULL;
	while(p/* + sizeof(MemoryUnit)*/ + unit <= ((uintptr_t)slab) + SLAB_SIZE){
		MemoryUnit *u = (MemoryUnit*)p;
		u->next = fl;
		fl = u;
		p = p/* + sizeof(MemoryUnit)*/ + unit;
	}
	slab->freeList = fl;
}

static void *allocateUnit(Slab *p){
	MemoryUnit *m = p->freeList;
	if(m == NULL)
		return NULL;
	p->freeList = m->next;
	p->usedCount++;
	return m->address;
}

static_assert((SLAB_SIZE & (SLAB_SIZE - 1)) == 0);
static Slab *freeUnit(void *address){
	uintptr_t a = (uintptr_t)address;
	MemoryUnit *u = address;
	Slab *p = (Slab*)(a - (a & (SLAB_SIZE - 1)));
	u->next = p->freeList;
	p->freeList = u;
	p->usedCount--;
	return p;
}

// slab Manager
static const size_t slabUnit[] = {
	16,
	32,
	64,
	128 - sizeof(Slab),
	256 - sizeof(Slab),
	512 - sizeof(Slab),
	1024 - sizeof(Slab),
	2048 - sizeof(Slab)
};
#define NUMBER_OF_SLAB_UNIT (LENGTH_OF(slabUnit))
static_assert(NUMBER_OF_SLAB_UNIT == 8);

typedef struct SlabManager{
	Spinlock lock;
	Slab *usableSlab[NUMBER_OF_SLAB_UNIT];
	Slab *usedSlab[NUMBER_OF_SLAB_UNIT];

	// allocate/release page
	PageAttribute pageAttribute;
	void *(*allocatePages)(size_t, PageAttribute);
	int (*releasePages)(void*);
}SlabManager;

static int findSlab(size_t size){
	int i;
	for(i = 0; 1; i++){
		if((unsigned)i >= NUMBER_OF_SLAB_UNIT){
			panic("error allocating memory");
			return i;
		}
		if(slabUnit[i] >= size){
			return i;
		}
	}
}

void *allocateSlab(SlabManager *m, size_t size){
	if(size >= slabUnit[NUMBER_OF_SLAB_UNIT - 1]){
		return m->allocatePages(CEIL(size, PAGE_SIZE), m->pageAttribute);
	}
	int i = findSlab(size);
	void *r = NULL;
	acquireLock(&(m->lock));
	do{
		Slab *p = m->usableSlab[i];
		if(p == NULL){
			p = (Slab*)m->allocatePages(SLAB_SIZE, m->pageAttribute);
			if(p == NULL){
				break;
			}
			initSlab(p, slabUnit[i]);
			ADD_TO_DQUEUE(p, m->usableSlab + i);
		}
		r = allocateUnit(p);
		// r can be NULL
		if(isTotallyUsed(p)){
			REMOVE_FROM_DQUEUE(p);
			ADD_TO_DQUEUE(p, m->usedSlab + i);
		}
	}while(0);
	releaseLock(&(m->lock));
	assert(r == NULL || ((uintptr_t)r) % MIN_BLOCK_SIZE != 0);
	return r;
}

void releaseSlab(SlabManager *m, void *address){
	//assert(getEFlags().bit.interrupt == 1);
	uintptr_t a = (uintptr_t)address;
	if(a % MIN_BLOCK_SIZE == 0){
		int ok = m->releasePages(address);
		assert(ok);
		return;
	}
	acquireLock(&m->lock);
	Slab *p = freeUnit(address);
	if(isTotallyFree(p)){
		REMOVE_FROM_DQUEUE(p);
		releaseLock(&m->lock);
		int ok = m->releasePages(p);
		assert(ok);
	}
	else{
		releaseLock(&m->lock);
	}
}

static SlabManager *createSlabManager(
	void*(*allocatePagesFunction)(size_t, PageAttribute),
	int (*releasePagesFunction)(void*),
	PageAttribute pageAttribute
){
	size_t unit;
	unsigned int i;
	for(i = 0; slabUnit[i] < sizeof(SlabManager); i++);
	assert(i < NUMBER_OF_SLAB_UNIT);
	unit = slabUnit[i];

	Slab *s = allocatePagesFunction(SLAB_SIZE, pageAttribute);
	initSlab(s, unit);
	if(s == NULL){
		return NULL;
	}
	SlabManager *m = allocateUnit(s);
	if(m == NULL){
		return NULL;
	}
	m->lock = initialSpinlock;

	for(i = 0; i < NUMBER_OF_SLAB_UNIT; i++){
		m->usableSlab[i] = NULL;
		m->usedSlab[i] = NULL;
		if(slabUnit[i] == unit){
			ADD_TO_DQUEUE(s, m->usableSlab + i);
		}
	}
	// allocate/release page
	m->pageAttribute = pageAttribute;
	m->allocatePages = allocatePagesFunction;
	m->releasePages = releasePagesFunction;
	return m;
}

SlabManager *createKernelSlabManager(void){
	return createSlabManager(allocateKernelPages, checkAndReleaseKernelPages, KERNEL_PAGE);
}
SlabManager *createUserSlabManager(void){
	return createSlabManager(systemCall_allocateHeap, systemCall_releaseHeap, USER_WRITABLE_PAGE);
}

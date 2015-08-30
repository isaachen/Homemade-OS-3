#include<std.h>
#include"common.h"
#include"memory.h"
#include"memory_private.h"
#include"assembly/assembly.h"
#include"multiprocessor/spinlock.h"

enum MemoryBlockStatus{
	MEMORY_FREE_OR_COVERED = 1, // may be free
	MEMORY_USING = 2, // must be using
	MEMORY_RELEASING = 3 // must be releasing
}__attribute__((__packed__));
// BlockManager functions

typedef uint16_t sizeorder_t;

typedef struct MemoryBlock{
	sizeorder_t sizeOrder;
	enum MemoryBlockStatus status;
	// for linear memory,
	// this value is indicates whether the linear block is mapped to physical pages.
	// for physical memory, it is 0
	uint8_t flags: 8;
	struct MemoryBlock**prev, *next;
}MemoryBlock;

static_assert(sizeof(MemoryBlock) == 12);

struct MemoryBlockManager{
	Spinlock lock;
	uintptr_t beginAddress;
	int initialBlockCount, blockCount, maxBlockCount;
	size_t freeSize;
	MemoryBlock *freeBlock[MAX_BLOCK_ORDER - MIN_BLOCK_ORDER + 1];
	MemoryBlock block[0];
};

static_assert(sizeof(*((MemoryBlockManager*)NULL)->block) == sizeof(MemoryBlock));

static void initMemoryBlock(MemoryBlock *mb){
	mb->sizeOrder = MIN_BLOCK_ORDER;
	mb->status = MEMORY_USING;
	mb->flags = 0;
	mb->next = NULL;
	mb->prev = NULL;
}

static uintptr_t getAddress(MemoryBlockManager *m, MemoryBlock *mb){
	return (((uintptr_t)(mb - m->block)) << MIN_BLOCK_ORDER) + m->beginAddress;
}

static int isInRange(MemoryBlockManager *m, uintptr_t address){
	if(address % MIN_BLOCK_SIZE != 0)
		return 0;
	if(address < m->beginAddress)
		return 0;
	if((address - m->beginAddress) / MIN_BLOCK_SIZE >= (unsigned)m->blockCount)
		return 0;
	return 1;
}

static MemoryBlock *getBlock(MemoryBlockManager *m, uintptr_t address){
	assert(isInRange(m, address));
	int i = (address - m->beginAddress) / MIN_BLOCK_SIZE;
	return &m->block[i];
}

uintptr_t getBeginAddress(MemoryBlockManager *m){
	return m->beginAddress;
}

// assume locked
static MemoryBlock *getBuddy(MemoryBlockManager *m, const MemoryBlock *b){
	assert(isAcquirable(&m->lock) == 0);
	int index = (b - m->block);
	int buddy = (index ^ (1 << (b->sizeOrder - MIN_BLOCK_ORDER)));
	if(buddy >= m->blockCount){
		return NULL;
	}
	return m->block + buddy;
}

static size_t ceilAllocateOrder(size_t s){
	if(s > MAX_BLOCK_SIZE)
		return MAX_BLOCK_ORDER + 1;
	size_t i;
	for(i = MIN_BLOCK_ORDER; (((size_t)1) << i) < s; i++);
	return i;
}

static uintptr_t allocateBlock_noLock(MemoryBlockManager *m, size_t *size, MemoryBlockFlags flags){
	assert(isAcquirable(&m->lock) == 0);
	// assert(size >= MIN_BLOCK_SIZE);
	size_t i = ceilAllocateOrder(*size), i2;
	if(i > MAX_BLOCK_ORDER){
		return UINTPTR_NULL;
	}
	for(i2 = i; 1; i2++){
		if(m->freeBlock[i2 - MIN_BLOCK_ORDER] != NULL)
			break;
		if(i2 == MAX_BLOCK_ORDER){
			return UINTPTR_NULL;
		}
	}
	MemoryBlock *const b = m->freeBlock[i2 - MIN_BLOCK_ORDER];
	REMOVE_FROM_DQUEUE(b);
	assert(b->status == MEMORY_FREE_OR_COVERED);
	b->status = MEMORY_USING;
	b->flags = flags;
	while(i2 != i){
		// split b and get buddy
		b->sizeOrder--;
		MemoryBlock *b2 = getBuddy(m, b);
		assert(b2 != NULL);
		assert(IS_IN_DQUEUE(b2) == 0 && b2->sizeOrder == b->sizeOrder);
		assert(b2->status == MEMORY_FREE_OR_COVERED);
		ADD_TO_DQUEUE(b2, &m->freeBlock[b2->sizeOrder - MIN_BLOCK_ORDER]);
		i2--;
	}
	m->freeSize -= (1 << i);
	(*size) = (size_t)(1 << i);
	uintptr_t ret = getAddress(m, b);
	assert(ret != UINTPTR_NULL);
	return ret;
}

uintptr_t allocateBlock(MemoryBlockManager *m, size_t *size, MemoryBlockFlags flags){
	uintptr_t r;
	acquireLock(&m->lock);
	r = allocateBlock_noLock(m, size, flags);
	releaseLock(&m->lock);
	return r;
}

size_t getAllocatedBlockSize(MemoryBlockManager *m, uintptr_t address){
	MemoryBlock *b = getBlock(m, address);
	return (1 << b->sizeOrder);
}

static int isReleasableBlock_noLock(MemoryBlockManager *m, MemoryBlock *b1){
#ifndef NDEBUG
	MemoryBlock *b2 = getBuddy(m, b1);
#endif
	switch(b1->status){
	case MEMORY_USING:
#ifndef NDEBUG
		assert(IS_IN_DQUEUE(b1) == 0);
		if(b2 != NULL){
			assert(b2->sizeOrder <= b1->sizeOrder);
		}
#endif
		return 1;
	case MEMORY_FREE_OR_COVERED:
#ifndef NDEBUG
		if(b2 != NULL){
			assert(IS_IN_DQUEUE(b1) || (b2 < b1 && b2->sizeOrder > b1->sizeOrder));
		}
		else{
			assert(IS_IN_DQUEUE(b1));
		}
#endif
	case MEMORY_RELEASING:
		return 0;
	default:
		assert(0);
		return 0;
	}
}

static int isReleasableAddress_noLock(MemoryBlockManager *m, uintptr_t address){
	if(isInRange(m, address) == 0){
		return 0;
	}
	return isReleasableBlock_noLock(m, getBlock(m, address));
}

int isReleasableAddress(MemoryBlockManager *m, uintptr_t address){
	int r = 0;
	acquireLock(&m->lock);
	r = isReleasableAddress_noLock(m, address);
	releaseLock(&m->lock);
	return r;
}

static void prepareReleaseBlock(MemoryBlock *b){
	assert(b->status == MEMORY_USING);
	b->status = MEMORY_RELEASING;
}

static void releaseBlock_noLock(MemoryBlockManager *m, MemoryBlock *b){
	m->freeSize += (1 << b->sizeOrder);
	assert(IS_IN_DQUEUE(b) == 0);
	assert(b->status == MEMORY_USING || b->status == MEMORY_RELEASING);
	b->status = MEMORY_FREE_OR_COVERED;
	b->flags = 0;
	while(b->sizeOrder < MAX_BLOCK_ORDER){
		MemoryBlock *buddy = getBuddy(m, b);
		if(buddy == NULL)
			break;
		assert(buddy->sizeOrder <= b->sizeOrder);
		if(IS_IN_DQUEUE(buddy) == 0 /*not free*/|| buddy->sizeOrder != b->sizeOrder/*partial free*/){
			break;
		}
		// merge
		//printk("%d %d\n",buddy->sizeOrder, b->sizeOrder);
		assert(buddy->status == MEMORY_FREE_OR_COVERED);
		REMOVE_FROM_DQUEUE(buddy);
		#ifndef NDEBUG
		{
			uintptr_t a1 = (uintptr_t)getAddress(m,buddy), a2=(uintptr_t)getAddress(m,b);
			assert((a1 > a2? a1-a2: a2-a1) == (size_t)(1 << b->sizeOrder));
		}
		#endif
		b = (getAddress(m, b) < getAddress(m, buddy)? b: buddy);
		b->sizeOrder++;
	}
	ADD_TO_DQUEUE(b, &m->freeBlock[b->sizeOrder - MIN_BLOCK_ORDER]);
}

void releaseBlock(MemoryBlockManager *m, uintptr_t address){
	acquireLock(&m->lock);
	assert(isReleasableAddress_noLock(m, address));
	releaseBlock_noLock(m, 	getBlock(m, address));
	releaseLock(&m->lock);
}

// if MEMORY_RELEASING or MEMORY_FREE, return 0
// if MEMORY_USING, return 1
static int isUsingBlock_noLock(MemoryBlockManager *m, uintptr_t address){
	const MemoryBlock *b1, *b2;
	b1 = getBlock(m, address);
	while(1){
		b2 = getBuddy(m, b1);
		// b1 is not covered by other blocks
		if(b2 == NULL || b2->sizeOrder <= b1->sizeOrder){
			break;
		}
		assert(b2 < b1);
		b1 = b2;
	}
	return (b1->status == MEMORY_USING);
}

const size_t minBlockManagerSize = sizeof(MemoryBlockManager);
const size_t maxBlockManagerSize = sizeof(MemoryBlockManager) +
	((0xffffffff / MIN_BLOCK_SIZE) + 1) * sizeof(MemoryBlock);

size_t getMaxBlockManagerSize(MemoryBlockManager *m){
	return sizeof(*m) + m->maxBlockCount * sizeof(*m->block);
}

int getMaxBlockCount(MemoryBlockManager *m){
	return m->maxBlockCount;
}

int getFreeBlockSize(MemoryBlockManager *m){
	return m->freeSize;
}

static void resetBlockArray(MemoryBlockManager *bm){
	int i;
	bm->blockCount = bm->initialBlockCount;
	bm->freeSize = 0;
	for(i = 0; i < bm->blockCount; i++){
		initMemoryBlock(&bm->block[i]);
	}
	for(i = 0; i <= MAX_BLOCK_ORDER - MIN_BLOCK_ORDER; i++){
		bm->freeBlock[i] = NULL;
	}
}

MemoryBlockManager *createMemoryBlockManager(
	uintptr_t manageBase,
	size_t manageSize,
	uintptr_t beginAddr,
	uintptr_t initEndAddr,
	uintptr_t maxEndAddr
){
	assert(manageBase % sizeof(uintptr_t) == 0);
	MemoryBlockManager *bm = (MemoryBlockManager*)manageBase;
	bm->lock = initialSpinlock;
	assert(beginAddr % MIN_BLOCK_SIZE == 0);
	//assert(UINTPTR_NULL < beginAddr || UINTPTR_NULL >= maxEndAddr);
	bm->beginAddress = beginAddr;

	bm->maxBlockCount = (maxEndAddr - bm->beginAddress) / MIN_BLOCK_SIZE;
	if(getMaxBlockManagerSize(bm) > manageSize){
		panic("buddy memory manager initialization error");
		return bm == NULL? ((void*)0xffffffff): NULL;
	}
	bm->initialBlockCount = (initEndAddr - bm->beginAddress) / MIN_BLOCK_SIZE;
	resetBlockArray(bm);
	return bm;
}

// linear memory manager

static int getExtendBlockCount(MemoryBlockManager *m, size_t size){
	assert(isAcquirable(&m->lock) == 0);
	size_t i = ceilAllocateOrder(size);
	// too large
	if(i > MAX_BLOCK_ORDER){
		return 0;
	}
	int addBlockCount = (1 << (i - MIN_BLOCK_ORDER));
	int newBlockCount = addBlockCount + CEIL(m->blockCount, addBlockCount);
	if(newBlockCount > m->maxBlockCount){
		return 0;
	}
	return newBlockCount - m->blockCount;
}

uintptr_t allocateOrExtendLinearBlock(LinearMemoryManager *m, size_t *size, MemoryBlockFlags flags){
	MemoryBlockManager *bm = m->linear;
	size_t l_size = *size;
	acquireLock(&bm->lock);
	uintptr_t linearAddress = allocateBlock_noLock(bm, &l_size, flags);
	if(linearAddress != UINTPTR_NULL){ // ok
		(*size) = l_size;
		goto alcOrExt_return;
	}

	int exCount = getExtendBlockCount(bm, *size);
	if(exCount == 0){ // error
		goto alcOrExt_return;
	}
	int newBlockcount = bm->blockCount + exCount;
	uintptr_t exPage = CEIL((uintptr_t)(bm->block + bm->blockCount), PAGE_SIZE);
	while(bm->blockCount < newBlockcount){
		MemoryBlock *const lastBlock = bm->block + bm->blockCount;
		if((uintptr_t)(lastBlock + 1) > exPage){
			// if mapPage_L fails, it calls _unmapPage which is not allowed in interrupt-disabled section
			// in order to prevent from calling _unmapPage, allocate only one page in each call
			int ok = mapPage_L(m->page, (void*)exPage, PAGE_SIZE, KERNEL_PAGE);
			if(!ok){
				break;
			}
			exPage += PAGE_SIZE;
		}
		bm->blockCount++;
		initMemoryBlock(lastBlock);
		// updated bm->blockCount is accessed here
		releaseBlock_noLock(bm, lastBlock);
	}

	l_size = *size;
	linearAddress = allocateBlock_noLock(bm, &l_size, flags);
	if(linearAddress != UINTPTR_NULL){
		(*size) = l_size;
	}
	alcOrExt_return:
	releaseLock(&bm->lock);
	return linearAddress;
}

int _checkAndUnmapLinearBlock(LinearMemoryManager *m, uintptr_t linearAddress, int releasePhysical){
	MemoryBlockManager *bm = m->linear;

	int r;
	size_t s;
	MemoryBlock *b;
	acquireLock(&bm->lock);
	r = isReleasableAddress_noLock(bm, linearAddress);
	if(r == 0){
		// r = 0;
		goto chkAndRls_return;
	}
	b = getBlock(bm, linearAddress);
	s = (1 << b->sizeOrder);
	if(s % PAGE_SIZE != 0){
		r = 0;
		panic("linear block must align to PAGE_SIZE");
		goto chkAndRls_return;
	}
	prepareReleaseBlock(b);
	// TODO: remove releasePhysicalĄ@parameter
	assert(b->flags == (unsigned)releasePhysical);
	releaseLock(&bm->lock);

	_unmapPage(m->page, m->physical, (void*)linearAddress, s, b->flags & WITH_PHYSICAL_PAGES_FLAG);

	acquireLock(&bm->lock);
	releaseBlock_noLock(bm, getBlock(bm, linearAddress));
	r = 1;
	chkAndRls_return:
	releaseLock(&bm->lock);
	return r;
}

// release every m->linear->block and reset m->linear->blockCount to initialBlockCount
// assume single thread
void releaseAllLinearBlocks(LinearMemoryManager *m){
	MemoryBlockManager *bm = m->linear;
	int i = 0;//bm->initialBlockCount;
	while(i < bm->blockCount){
		assert(bm->block[i].status != MEMORY_RELEASING);
		_checkAndUnmapLinearBlock(m, getAddress(bm, bm->block + i), bm->block[i].flags);
		// no lock
		// no matter the block is free, using, or covered, adding the block size does not skip any using block
		i += (1 << bm->block[i].sizeOrder) / MIN_BLOCK_SIZE;
	}
	assert(i == bm->blockCount);
	// see allocateOrExtendLinearBlock
	uintptr_t rlsPageBegin = CEIL((uintptr_t)(bm->block + bm->initialBlockCount), PAGE_SIZE);
	uintptr_t rlsPageEnd = CEIL((uintptr_t)(bm->block + bm->blockCount), PAGE_SIZE);
	unmapPage_L(m->page, (void*)rlsPageBegin, rlsPageEnd - rlsPageBegin);
	resetBlockArray(bm);
}

static PhysicalAddress checkAndTranslateBlock(LinearMemoryManager *m, uintptr_t linearAddress){
	MemoryBlockManager *bm = m->linear;
	PhysicalAddress p = {UINTPTR_NULL};
	if(isInRange(bm, linearAddress) == 0){
		return p;
	}
	acquireLock(&bm->lock);
	if(isUsingBlock_noLock(bm, linearAddress)){
		p = translateExistingPage(m->page, (void*)linearAddress);
		assert(p.value != UINTPTR_NULL);
	}
	releaseLock(&bm->lock);
	return p;
}

PhysicalAddress checkAndTranslatePage(LinearMemoryManager *m, void *linearAddress){
	return checkAndTranslateBlock(m, (uintptr_t)linearAddress);
}

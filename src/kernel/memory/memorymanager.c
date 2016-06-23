#include"common.h"
#include"kernel.h"
#include"memory.h"
#include"assembly/assembly.h"
#include"multiprocessor/spinlock.h"
#include"memory_private.h"

// BIOS address range functions

enum AddressRangeType{
	USABLE = 1,
	RESERVED = 2,
	ACPI_RECLAIMABLE = 3,
	ACPI_NVS = 4,
	BAD_MEMORY = 5
};
static_assert(sizeof(enum AddressRangeType) == 4);

typedef struct AddressRange{
	uint64_t base;
	uint64_t size;
	enum AddressRangeType type;
	uint32_t extra;
}AddressRange;
static_assert(sizeof(AddressRange) == 24);

extern const AddressRange *const addressRange;
extern const int addressRangeCount;

#define OS_MAX_ADDRESS (((uintptr_t)0xffffffff) - ((uintptr_t)0xffffffff) % MIN_BLOCK_SIZE)
static_assert(OS_MAX_ADDRESS % MIN_BLOCK_SIZE == 0);

static uintptr_t findMaxAddress(void){
	int i;
	uint64_t maxAddr = 0;
	// kprintf("%d memory address ranges\n", addressRangeCount);
	for(i = 0; i < addressRangeCount; i++){
		const struct AddressRange *ar = addressRange + i;
		/*
		printk("type: %d base: %x %x size: %x %x\n", ar->type,
		(uint32_t)(ar->base >> 32), (uint32_t)(ar->base & 0xffffffff),
		(uint32_t)(ar->size >> 32), (uint32_t)(ar->size & 0xffffffff));
		 */
		if(ar->type == USABLE && ar->size != 0 &&
			maxAddr < ar->base + ar->size - 1){
			maxAddr = ar->base + ar->size - 1;
		}
	}
	if(maxAddr >= OS_MAX_ADDRESS){
		return OS_MAX_ADDRESS;
	}
	return maxAddr + 1;
}

// global memory manager
static SlabManager *kernelSlab = NULL;

void *allocateKernelMemory(size_t size){
	assert(kernelSlab != NULL);
	return allocateSlab(kernelSlab, size);
}

void releaseKernelMemory(void *linearAddress){
	assert(kernelSlab != NULL);
	releaseSlab(kernelSlab, linearAddress);
}

int checkAndReleaseKernelPages(void *linearAddress){
	return checkAndReleasePages(kernelLinear, linearAddress);
}

// LinearMemoryManager

void *mapPages(LinearMemoryManager *m, PhysicalAddress physicalAddress, size_t size, PageAttribute attribute){
	assert(size % PAGE_SIZE == 0);
	assert(m->page != NULL && m->linear != NULL);
	// linear
	uintptr_t linearAddress = allocateLinearBlock(m, size);
	EXPECT(linearAddress != INVALID_PAGE_ADDRESS);
	assert(linearAddress % PAGE_SIZE == 0 && size != 0);
	// assume linear memory manager and page table are consistent
	// linearAddess[0 ~ size] are available
	// it is safe to map pages in the range
	int result = _mapPage_LP(m->page, m->physical, (void*)linearAddress, physicalAddress, size, attribute);
	EXPECT(result == 1);

	commitAllocatingLinearBlock(m, linearAddress);
	return (void*)linearAddress;

	ON_ERROR;
	releaseLinearBlock(m->linear, (uintptr_t)linearAddress);
	ON_ERROR;
	return NULL;
}
/*
void *mapExistingPages(
	LinearMemoryManager *dst, PageManager *src,
	uintptr_t srcLinear, size_t size,
	PageAttribute attribute, PageAttribute srcHasAttribute
){
	size_t l_size = size;
	uintptr_t dstLinear = allocateLinearBlock(dst, &l_size);
	EXPECT(dstLinear != INVALID_PAGE_ADDRESS);
	int ok = _mapExistingPages_L(
		dst->physical, dst->page, src,
		(void*)dstLinear, srcLinear, size,
		attribute, srcHasAttribute);
	EXPECT(ok);

	commitAllocatingLinearBlock(dst, dstLinear);
	return (void*)dstLinear;

	ON_ERROR;
	releaseLinearBlock(dst->linear, dstLinear);
	ON_ERROR;
	return NULL;
}
*/
void *checkAndMapExistingPages(
	LinearMemoryManager *dst, LinearMemoryManager *src,
	uintptr_t srcLinear, size_t size,
	PageAttribute attribute, PageAttribute srcHasAttribute
){
	if(isKernelLinearAddress(srcLinear)){ // XXX:
		src = kernelLinear;
	}
	assert(dst == kernelLinear || src == kernelLinear);
	uintptr_t dstLinear = allocateLinearBlock(dst, size);
	EXPECT(dstLinear != INVALID_PAGE_ADDRESS);
	uintptr_t s;
	for(s = 0; s < size; s += PAGE_SIZE){
		PhysicalAddress srcPhysical = checkAndReservePage(src, (void*)(srcLinear + s), srcHasAttribute);
		if(srcPhysical.value == INVALID_PAGE_ADDRESS)
			break;
		int ok = _mapPage_LP(dst->page, dst->physical,
			(void*)(dstLinear + s), srcPhysical, PAGE_SIZE, attribute);
		releaseReservedPage(src, srcPhysical);
		if(ok == 0)
			break;
	}
	EXPECT(s == size);

	commitAllocatingLinearBlock(dst, dstLinear);
	return (void*)dstLinear;

	ON_ERROR;
	_unmapPage_LP(dst->page, dst->physical, (void*)dstLinear, s);
	releaseLinearBlock(dst->linear, dstLinear);
	ON_ERROR;
	return NULL;
}

void unmapPages(LinearMemoryManager *m, void *linearAddress){
	size_t s = getAllocatedBlockSize(m->linear, (uintptr_t)linearAddress);
	_unmapPage_LP(m->page, m->physical, linearAddress, s);
	releaseLinearBlock(m->linear, (uintptr_t)linearAddress);
}

int checkAndUnmapPages(LinearMemoryManager *m, void *linearAddress){
	assert(m->page != NULL && m->linear != NULL);
	return checkAndReleaseLinearBlock(m, (uintptr_t)linearAddress);
}

void releaseReservedPage(LinearMemoryManager *m, PhysicalAddress physicalAddress){
	releasePhysicalBlock(m->physical, physicalAddress.value);
}

/* unused functions
static void _deleteBufferPhysicalAddressArray(PhysicalAddressArray *pa, uintptr_t paLength){
	while(paLength != 0){
		paLength--;
		_releasePhysicalPages(pa->physicalManager, pa->address[paLength]);
	}
	DELETE(pa);
}

PhysicalAddressArray *checkAndReservePages(LinearMemoryManager *lm, const void *linearAddress, uintptr_t size){
	EXPECT(((uintptr_t)linearAddress) % PAGE_SIZE == 0 && size % PAGE_SIZE == 0);
	const uintptr_t pageLength = (size) / PAGE_SIZE;
	PhysicalAddressArray *pa = allocateKernelMemory(sizeof(*pa) + sizeof(pa->address[0]) * pageLength);
	EXPECT(pa != NULL);
	pa->length = pageLength;
	pa->physicalManager = lm->physical;
	uintptr_t a = 0;
	for(a = 0; a < pageLength; a++){
		// TODO: attribute
		pa->address[a] = checkAndReservePage(lm, (void*)(((uintptr_t)linearAddress) + a * PAGE_SIZE), 0);
		if(pa->address[a].value == INVALID_PAGE_ADDRESS)
			break;
	}
	EXPECT(a * PAGE_SIZE == size);

	return pa;
	ON_ERROR;
	_deleteBufferPhysicalAddressArray(pa, a);
	ON_ERROR;
	ON_ERROR;
	return NULL;
}

void deletePhysicalAddressArray(PhysicalAddressArray *pa){
	_deleteBufferPhysicalAddressArray(pa, pa->length);
}

void *mapReservedPages(LinearMemoryManager *lm, const PhysicalAddressArray *pa, PageAttribute attribute){
	size_t l_size = PAGE_SIZE * pa->length;
	uintptr_t linearAddress = allocateLinearBlock(lm, &l_size);
	EXPECT(linearAddress != INVALID_PAGE_ADDRESS);
	uintptr_t a;
	for(a = 0; a < pa->length; a++){
		if(_mapPage_LP(lm->page, pa->physicalManager,
			(void*)(linearAddress + a * PAGE_SIZE), pa->address[a], PAGE_SIZE, attribute) == 0)
			break;;
	}
	EXPECT(a == pa->length);
	commitAllocatingLinearBlock(lm, linearAddress);
	return (void*)linearAddress;

	ON_ERROR;
	_unmapPage_LP(lm->page, pa->physicalManager, (void*)linearAddress, a * PAGE_SIZE);
	releaseLinearBlock(lm->linear, linearAddress);
	ON_ERROR;
	return NULL;
}
*/
static void *_allocatePages(LinearMemoryManager *m, size_t size, int contiguous, PageAttribute attribute){
	// linear
	uintptr_t linearAddress = allocateLinearBlock(m, size);
	EXPECT(linearAddress != INVALID_PAGE_ADDRESS);
	// physical
	int ok;
	if(contiguous){
		ok = _mapContiguousPage_L(m->page, m->physical, (void*)linearAddress, size, attribute);
	}
	else{
		ok = _mapPage_L(m->page, m->physical, (void*)linearAddress, size, attribute);
	}
	EXPECT(ok);

	commitAllocatingLinearBlock(m, linearAddress);
	return (void*)linearAddress;

	ON_ERROR;
	releaseLinearBlock(m->linear, (uintptr_t)linearAddress);
	ON_ERROR;
	return NULL;
}

void *allocatePages(LinearMemoryManager *m, size_t size, PageAttribute attribute){
	return _allocatePages(m, size, 0, attribute);
}

void *allocateContiguousPages(LinearMemoryManager *m, size_t size, PageAttribute attribute){
	return _allocatePages(m, size, 1, attribute);
}

void *allocateKernelPages(size_t size, PageAttribute attribute){
	return allocatePages(kernelLinear, size, attribute);
}

/*
void releasePages(LinearMemoryManager *m, void *linearAddress){
	size_t s = getAllocatedBlockSize(m->linear, (uintptr_t)linearAddress);
	_unmapPage_L(m->page, m->physical, linearAddress, s);
	releaseBlock(m->linear, (uintptr_t)linearAddress);
}
*/
int checkAndReleasePages(LinearMemoryManager *m, void *linearAddress){
	return checkAndReleaseLinearBlock(m, (uintptr_t)linearAddress);
}

/*
size_t getKernelMemoryUsage(){
}
*/

// initialize kernel MemoryBlockManager
static int isUsableInAddressRange(
	uintptr_t address,
	const AddressRange *arArray1, int arLength1,
	const AddressRange *arArray2, int arLength2
){
	int isInUnusable = 0, isInUsable = 0;
	int i;
	for(i = 0; isInUnusable == 0 && i < arLength1 + arLength2; i++){
		const AddressRange *ar = (i < arLength1? arArray1 + i: arArray2 + (i - arLength1));
		if( // completely covered by usable range
			ar->type == USABLE &&
			(ar->base <= address && ar->base + ar->size >= address + MIN_BLOCK_SIZE)
		){
			isInUsable = 1;
		}
		if( // partially covered by unusable range
			ar->type != USABLE &&
			!(ar->base >= address + MIN_BLOCK_SIZE || ar->base + ar->size <= address)
		){
			isInUnusable = 1;
		}
	}
	return (isInUnusable == 0 && isInUsable == 1);
}

static PhysicalMemoryBlockManager *initKernelPhysicalBlock(
	uintptr_t manageBase, uintptr_t *manageBegin, uintptr_t manageEnd,
	uintptr_t minAddress, uintptr_t maxAddress
){
	PhysicalMemoryBlockManager *m = createPhysicalMemoryBlockManager(
		*manageBegin, manageEnd - *manageBegin,
		minAddress, maxAddress
	);
	const AddressRange extraAR[1] = {
		{manageBase - KERNEL_LINEAR_BEGIN, manageEnd - manageBase, RESERVED, 0}
	};

	int b;
	const int bCount = getPhysicalBlockCount(m);
	for(b = 0; b < bCount; b++){
		uintptr_t address = minAddress + b * MIN_BLOCK_SIZE;
		assert(address + MIN_BLOCK_SIZE > address); // not overflow
		if(isUsableInAddressRange(address, addressRange, addressRangeCount, extraAR, LENGTH_OF(extraAR))){
			releasePhysicalBlock(m, address);
		}
	}
	(*manageBegin) += getPhysicalBlockManagerSize(m);
	return m;
}

static LinearMemoryBlockManager *initKernelLinearBlock(
	uintptr_t manageBase, uintptr_t *manageBegin, uintptr_t manageEnd,
	uintptr_t minAddress, uintptr_t maxAddress
){
	uintptr_t newManageBegin = *manageBegin;
	// see initKernelPhysicalBlock
	LinearMemoryBlockManager *m = createLinearBlockManager(
		newManageBegin, manageEnd - newManageBegin,
		minAddress, maxAddress, maxAddress
	);
	const AddressRange extraAR[2] = {
		{manageBase, manageEnd - manageBase, RESERVED, 0},
		{minAddress, maxAddress, USABLE, 0}
	};

	int b;
	const int bCount = getMaxBlockCount(m);
	for(b = 0; b < bCount; b++){
		uintptr_t address = minAddress + b * MIN_BLOCK_SIZE;
		assert(address + MIN_BLOCK_SIZE > address);
		if(isUsableInAddressRange(address, NULL, 0, extraAR, LENGTH_OF(extraAR))){
			releaseLinearBlock(m, address);
		}
	}
	newManageBegin += getMaxLinearBlockManagerSize(m);
	assert(evaluateLinearBlockEnd(*manageBegin, minAddress, maxAddress) == newManageBegin);
	(*manageBegin) = newManageBegin;
	return m;
}


static LinearMemoryManager _kernelLinear;
LinearMemoryManager *kernelLinear = &_kernelLinear;

void initKernelMemory(void){
	assert(kernelLinear->linear == NULL && kernelLinear->page == NULL && kernelLinear->physical == NULL);
	// reserved... are linear address
	const uintptr_t reservedBase = KERNEL_LINEAR_BEGIN;
	uintptr_t reservedBegin = reservedBase + (1 << 20);
	uintptr_t reservedEnd = reservedBase + (23 << 20);
	//IMPROVE: how to reduce reserved memory
	// at most 4GB / 4096 * 16 = 16MB
	kernelLinear->physical = initKernelPhysicalBlock(
		reservedBase, &reservedBegin, reservedEnd,
		0, findMaxAddress()
	);
	// page table = 4MB
	// page directory = 4KB
	kernelLinear->page = initKernelPageTable(reservedBase, &reservedBegin, reservedEnd);
	// call initKernelConsole which enables printk to help debugging
	// initKernelConsole();
	// fixed (4GB - KERNEL_LINEAR_BEGIN) / 4096 * 20 = 1280KB
	kernelLinear->linear = initKernelLinearBlock(
		reservedBase, &reservedBegin, reservedEnd,
		KERNEL_LINEAR_BEGIN, KERNEL_LINEAR_END
	);
	kernelSlab = createKernelSlabManager();
}


#ifndef NDEBUG
#define TEST_N (90)
void testMemoryManager(void){
	uint8_t *p[TEST_N];
	int si[TEST_N];
	unsigned int r;
	int a, b, c;
	r=MIN_BLOCK_SIZE + 351;
	for(b=0;b<10;b++){
		for(a=0;a<TEST_N;a++){
			si[a]=r;
			p[a]=allocateKernelMemory(r);
			if(p[a] == NULL){
				// printk("a = %d, r = %d p[a] = %x\n", a, r, p[a]);
			}
			else{
				for(c=0;c<si[a]&&c<100;c++){
					p[a][c] =
					p[a][si[a]-c-1]= a+1;
				}
			}
			//r = 1 + (r*7 + 3) % (30 - 1);
			r = (r*79+3);
			if(r%5<3) r = r % 2048;
			else r = (r*17) % (MAX_BLOCK_SIZE - MIN_BLOCK_SIZE) + MIN_BLOCK_SIZE;
		}
		for(a=0;a<TEST_N;a++){
			int a2 = (a+r)%TEST_N;
			if(p[a2] == NULL)continue;
			for(c = 0; c < si[a2] && c < 100; c++){
				if(p[a2][c] != a2+1 || p[a2][si[a2]-c-1] != a2+1){
					//printk("%x %x %d %d %d %d\n", p[a2], p[p[a2][c]-1],si[p[a2][c]-1], p[a2][c], p[a2][si[a2]-c-1], a2+1);
					panic("memory test failed");
				}
			}
			releaseKernelMemory((void*)p[a2]);
		}
	}
	//printk("test memory: ok\n");
	//printk("%x %x %x %x %x\n",a1,a2,a3, MIN_BLOCK_SIZE+(uintptr_t)a3,a4);
}


void testMemoryManager2(void){
	PhysicalAddress p[TEST_N];
	int a;
	unsigned int r=38;
	for(a=0;a<TEST_N;a++){
		size_t s=r*MIN_BLOCK_SIZE;
		r=(r*31+5)%197;
		p[a].value=allocatePhysicalBlock(kernelLinear->physical, s, s);
		if(p[a].value == INVALID_PAGE_ADDRESS)continue;
	}
	for(a=0;a<TEST_N;a++){
		if(p[a].value == INVALID_PAGE_ADDRESS)continue;
		releasePhysicalBlock(kernelLinear->physical, p[a].value);
	}
}

void testMemoryManager3(void){
	void *m1 = allocateKernelMemory(MIN_BLOCK_SIZE * 4);
	void *m2 = allocateKernelMemory(MIN_BLOCK_SIZE * 4);
	releaseKernelMemory(m2);
	int a;
	for(a=0; a<MIN_BLOCK_SIZE*4; a+=MIN_BLOCK_SIZE){
		PhysicalAddress chk;
		chk = checkAndTranslatePage(kernelLinear, (void*)(((uintptr_t)m1)+a));
		assert(chk.value != INVALID_PAGE_ADDRESS);
		chk = checkAndTranslatePage(kernelLinear, (void*)(((uintptr_t)m2)+a));
		assert(chk.value == INVALID_PAGE_ADDRESS);
	}
	releaseKernelMemory(m1);
}

void testMemoryManager4(void){
	uint8_t *a[TEST_N];
	size_t s[TEST_N];
	int i;
	unsigned r = 3893;
	for(i = 0; i < TEST_N; i++){
		r = (r * 47 + 5) % 10001;
		s[i] = r;
		a[i] = allocateContiguousPages(kernelLinear, PAGE_SIZE * r, KERNEL_PAGE);
		if(a[i] == NULL)
			continue;
		size_t j;
		for(j = 0; j < s[i]; j++){
			a[i][j * PAGE_SIZE] = (uint8_t)i;
		}
	}
	for(i = 0; i < TEST_N; i++){
		int i2=(i*7)%TEST_N;
		if(a[i2] == NULL)
			continue;
		size_t j;
		for(j = 0; j < s[i2]; j++){
			uint8_t *a2 = &a[i2][j * PAGE_SIZE];
			assert(a2[0] == (uint8_t)i2);
			a2[0]= 0;
		}
		for(j = 0; j < s[i2]; j++){
			uint8_t *a2 = &a[i2][j * PAGE_SIZE];
			if(j == 0 && checkAndReleasePages(kernelLinear, a2) == 0){
				assert(0);
			}
			if(checkAndReleasePages(kernelLinear, a2) != 0){
				assert(0);
			}
		}
	}

}

#undef TEST_N
#endif

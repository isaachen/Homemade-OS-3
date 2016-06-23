#ifndef HANDLER_H_INCLUDED
#define HANDLER_H_INCLUDED
#include<std.h>

#include "../../lib/systemcall.h"
#include"assembly/assembly.h"

typedef struct InterruptVector InterruptVector;
// handler parameter, see interruptentry.asm

typedef struct{
	/*const */uint32_t
	gs, fs, es, ds;
	uint32_t
	edi, esi, ebp,
	ebx, edx, ecx, eax;
}GeneralRegisters;

typedef struct InterruptParam{
	uintptr_t argument; // pushed by os
	InterruptVector *vector;
	GeneralRegisters regs;
	// errorCode is pushed by cpu or os
	uint32_t errorCode;
	// the following are pushed by cpu
	uint32_t eip, cs;
	EFlags eflags;
	// pushed if privilege changed
	uint32_t esp, ss;
	// pushed if cpu was in virtual 8086 mode
	uint32_t es8086, ds8086, fs8086, gs8086;
}InterruptParam;
#define PRIVILEGE_UNCHANGED_INTERRUPT_PARAM_SIZE ((size_t)&((InterruptParam*)0)->esp)
#define PRIVILEGE_CHANGED_INTERRUPT_PARAM_SIZE ((size_t)&((InterruptParam*)0)->es8086)
#define VIRTUAL_8086_INTERRUPT_PARAM_SIZE ((size_t)sizeof(InterruptParam))

// handler
void defaultInterruptHandler(InterruptParam *param);

typedef void (*InterruptHandler)(InterruptParam *param);
typedef int (*ChainedInterruptHandler)(const InterruptParam *param);
typedef struct InterruptTable InterruptTable;

// vector
enum ReservedInterruptVector{
	DIVIDE_BY_ZERO_FAULT = 0,
	DEBUG_TRAP = 1,
	NON_MASKABLE_INTERRUPT = 2,
	BREAKPOINT_TRAP = 3,
	TRAP = 4,
	BOUND_FAULT = 5,
	INVALID_OPCODE_FAULT = 6,
	NO_FLOATING_FAULT = 7,
	DOUBLE_FAULT = 8,
	// 9 is reserved after i386
	// FLOATING_SEGMENT_FAULT = 9,
	INVALID_TSS_FAULT = 10,
	NO_SEGMENT_FAULT = 11,
	STACK_SEGMENT_FAULT = 12,
	GENERAL_PROTECTION_FAULT = 13,
	PAGE_FAULT = 14,
	// 15 is reserved,
	FLOATING_FAULT = 16,
	ALIGNMENT_FAULT = 17,
	MACHINE_CHECK_ERROR = 18,
	SSE_FAULT = 19,
	VIRTUALIZATION_FAULT = 20,
	// 21~31 are reserved
	BEGIN_GENERAL_VECTOR = 32,
	END_GENERAL_VECTOR = 96,

	SYSTEM_CALL = SYSTEM_CALL_VECTOR, // 126
	SPURIOUS_INTERRUPT = 127
};
#define SYSTEM_CALL_VECTOR_STRING "126"

InterruptVector *registerGeneralInterrupt(InterruptTable *t, InterruptHandler handler, uintptr_t arg);
InterruptVector *registerInterrupt(
	InterruptTable *t,
	enum ReservedInterruptVector,
	InterruptHandler handler,
	uintptr_t arg
);
InterruptVector *registerIRQs(InterruptTable *t, int irqBegin, int irqCount);

// for IRQ
int addHandler(InterruptVector *vector, ChainedInterruptHandler handler, uintptr_t arg);
int removeHandler(InterruptVector *vector, ChainedInterruptHandler handler, uintptr_t arg);
// for general or reserved
void replaceHandler(InterruptVector *v, InterruptHandler *handler, uintptr_t *arg);
void setHandler(InterruptVector *v, InterruptHandler handler, uintptr_t arg);

uint8_t toChar(InterruptVector *v);
int getIRQ(InterruptVector *v);

enum IRQ{
	TIMER_IRQ = 0,
	KEYBOARD_IRQ = 1,
	SLAVE_IRQ = 2,
	FLOPPY_IRQ = 6,
	MOUSE_IRQ = 12
};

#endif

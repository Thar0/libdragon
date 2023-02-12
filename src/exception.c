/**
 * @file exception.c
 * @brief Exception Handler
 * @ingroup exceptions
 */
#include "exception.h"
#include "console.h"
#include "n64sys.h"
#include "debug.h"
#include "regsinternal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/**
 * @defgroup exceptions Exception Handler
 * @ingroup lowlevel
 * @brief Handle hardware-generated exceptions.
 *
 * The exception handler traps exceptions generated by hardware.  This could
 * be an invalid instruction or invalid memory access exception or it could
 * be a reset exception.  In both cases, a handler registered with 
 * #register_exception_handler will be passed information regarding the
 * exception type and relevant registers.
 *
 * @{
 */

/** @brief Maximum number of reset handlers that can be registered. */
#define MAX_RESET_HANDLERS 4

/** @brief Unhandled exception handler currently registered with exception system */
static void (*__exception_handler)(exception_t*) = exception_default_handler;
/** @brief Base register offset as defined by the interrupt controller */
extern volatile reg_block_t __baseRegAddr;

/**
 * @brief Register an exception handler to handle exceptions
 *
 * The registered handle is responsible for clearing any bits that may cause
 * a re-trigger of the same exception and updating the EPC. An important
 * example is the cause bits (12-17) of FCR31 from cop1. To prevent
 * re-triggering the exception they should be cleared by the handler.
 *
 * To manipulate the registers, update the values in the exception_t struct.
 * They will be restored to appropriate locations when returning from the
 * handler. Setting them directly will not work as expected as they will get
 * overwritten with the values pointed by the struct.
 *
 * There is only one exception to this, cr (cause register) which is also
 * modified by the int handler before the saved values are restored thus it
 * is only possible to update it through C0_WRITE_CR macro if it is needed.
 * This shouldn't be necessary though as they are already handled by the
 * library.
 *
 * k0 ($26), k1 ($27) are not saved/restored and will not be available in the
 * handler. Theoretically we can exclude s0-s7 ($16-$23), and gp ($28) to gain
 * some performance as they are already saved by GCC when necessary. The same
 * is true for sp ($29) and ra ($31) but current interrupt handler manipulates
 * them via allocating a new stack and doing a jal. Similarly floating point
 * registers f21-f31 are callee-saved. In the future we may consider removing
 * them from the save state for interrupts (but not for exceptions)
 *
 * @param[in] cb
 *            Callback function to call when exceptions happen
 */
exception_handler_t register_exception_handler( exception_handler_t cb )
{
	exception_handler_t old = __exception_handler;
	__exception_handler = cb;
	return old;
}

/**
 * @brief Default exception handler.
 * 
 * This handler is installed by default for all exceptions. It initializes
 * the console and dump the exception state to the screen, including the value
 * of all GPR/FPR registers. It then calls abort() to abort execution.
 */
void exception_default_handler(exception_t* ex) {
	uint32_t cr = ex->regs->cr;
	uint32_t sr = ex->regs->sr;
	uint32_t fcr31 = ex->regs->fc31;

	switch(ex->code) {
		case EXCEPTION_CODE_STORE_ADDRESS_ERROR:
		case EXCEPTION_CODE_LOAD_I_ADDRESS_ERROR:
		case EXCEPTION_CODE_TLB_MODIFICATION:
		case EXCEPTION_CODE_TLB_STORE_MISS:
		case EXCEPTION_CODE_TLB_LOAD_I_MISS:
		case EXCEPTION_CODE_COPROCESSOR_UNUSABLE:
		case EXCEPTION_CODE_FLOATING_POINT:
		case EXCEPTION_CODE_WATCH:
		case EXCEPTION_CODE_ARITHMETIC_OVERFLOW:
		case EXCEPTION_CODE_TRAP:
		case EXCEPTION_CODE_I_BUS_ERROR:
		case EXCEPTION_CODE_D_BUS_ERROR:
		case EXCEPTION_CODE_SYS_CALL:
		case EXCEPTION_CODE_BREAKPOINT:
		case EXCEPTION_CODE_INTERRUPT:
		default:
		break;
	}

	console_init();
	console_set_debug(true);
	console_set_render_mode(RENDER_MANUAL);

	fprintf(stdout, "%s exception at PC:%08lX\n", ex->info, (uint32_t)(ex->regs->epc + ((cr & C0_CAUSE_BD) ? 4 : 0)));

	fprintf(stdout, "CR:%08lX (COP:%1lu BD:%u)\n", cr, C0_GET_CAUSE_CE(cr), (bool)(cr & C0_CAUSE_BD));
	fprintf(stdout, "SR:%08lX FCR31:%08X BVAdr:%08lX \n", sr, (unsigned int)fcr31, C0_BADVADDR());
	fprintf(stdout, "----------------------------------------------------------------");
	fprintf(stdout, "FPU IOP UND OVE DV0 INV NI | INT sw0 sw1 ex0 ex1 ex2 ex3 ex4 tmr");
	fprintf(stdout, "Cause%2u %3u %3u %3u %3u%3u | Cause%2u %3u %3u %3u %3u %3u %3u %3u",
		(bool)(fcr31 & C1_CAUSE_INEXACT_OP),
		(bool)(fcr31 & C1_CAUSE_UNDERFLOW),
		(bool)(fcr31 & C1_CAUSE_OVERFLOW),
		(bool)(fcr31 & C1_CAUSE_DIV_BY_0),
		(bool)(fcr31 & C1_CAUSE_INVALID_OP),
		(bool)(fcr31 & C1_CAUSE_NOT_IMPLEMENTED),

		(bool)(cr & C0_INTERRUPT_0),
		(bool)(cr & C0_INTERRUPT_1),
		(bool)(cr & C0_INTERRUPT_RCP),
		(bool)(cr & C0_INTERRUPT_3),
		(bool)(cr & C0_INTERRUPT_4),
		(bool)(cr & C0_INTERRUPT_5),
		(bool)(cr & C0_INTERRUPT_6),
		(bool)(cr & C0_INTERRUPT_TIMER)
	);
	fprintf(stdout, "En  %3u %3u %3u %3u %3u  - | MASK%3u %3u %3u %3u %3u %3u %3u %3u",
		(bool)(fcr31 & C1_ENABLE_INEXACT_OP),
		(bool)(fcr31 & C1_ENABLE_UNDERFLOW),
		(bool)(fcr31 & C1_ENABLE_OVERFLOW),
		(bool)(fcr31 & C1_ENABLE_DIV_BY_0),
		(bool)(fcr31 & C1_ENABLE_INVALID_OP),

		(bool)(sr & C0_INTERRUPT_0),
		(bool)(sr & C0_INTERRUPT_1),
		(bool)(sr & C0_INTERRUPT_RCP),
		(bool)(sr & C0_INTERRUPT_3),
		(bool)(sr & C0_INTERRUPT_4),
		(bool)(sr & C0_INTERRUPT_5),
		(bool)(sr & C0_INTERRUPT_6),
		(bool)(sr & C0_INTERRUPT_TIMER)
	);

	fprintf(stdout, "Flags%2u %3u %3u %3u %3u  - |\n",
		(bool)(fcr31 & C1_FLAG_INEXACT_OP),
		(bool)(fcr31 & C1_FLAG_UNDERFLOW),
		(bool)(fcr31 & C1_FLAG_OVERFLOW),
		(bool)(fcr31 & C1_FLAG_DIV_BY_0),
		(bool)(fcr31 & C1_FLAG_INVALID_OP)
	);

	fprintf(stdout, "-------------------------------------------------GP Registers---");

	fprintf(stdout, "z0:%08lX ",		(uint32_t)ex->regs->gpr[0]);
	fprintf(stdout, "at:%08lX ",		(uint32_t)ex->regs->gpr[1]);
	fprintf(stdout, "v0:%08lX ",		(uint32_t)ex->regs->gpr[2]);
	fprintf(stdout, "v1:%08lX ",		(uint32_t)ex->regs->gpr[3]);
	fprintf(stdout, "a0:%08lX\n",		(uint32_t)ex->regs->gpr[4]);
	fprintf(stdout, "a1:%08lX ",		(uint32_t)ex->regs->gpr[5]);
	fprintf(stdout, "a2:%08lX ",		(uint32_t)ex->regs->gpr[6]);
	fprintf(stdout, "a3:%08lX ",		(uint32_t)ex->regs->gpr[7]);
	fprintf(stdout, "t0:%08lX ",		(uint32_t)ex->regs->gpr[8]);
	fprintf(stdout, "t1:%08lX\n",		(uint32_t)ex->regs->gpr[9]);
	fprintf(stdout, "t2:%08lX ",		(uint32_t)ex->regs->gpr[10]);
	fprintf(stdout, "t3:%08lX ",		(uint32_t)ex->regs->gpr[11]);
	fprintf(stdout, "t4:%08lX ",		(uint32_t)ex->regs->gpr[12]);
	fprintf(stdout, "t5:%08lX ",		(uint32_t)ex->regs->gpr[13]);
	fprintf(stdout, "t6:%08lX\n",		(uint32_t)ex->regs->gpr[14]);
	fprintf(stdout, "t7:%08lX ",		(uint32_t)ex->regs->gpr[15]);
	fprintf(stdout, "t8:%08lX ",		(uint32_t)ex->regs->gpr[24]);
	fprintf(stdout, "t9:%08lX ",		(uint32_t)ex->regs->gpr[25]);

	fprintf(stdout, "s0:%08lX ",		(uint32_t)ex->regs->gpr[16]);
	fprintf(stdout, "s1:%08lX\n",		(uint32_t)ex->regs->gpr[17]);
	fprintf(stdout, "s2:%08lX ",		(uint32_t)ex->regs->gpr[18]);
	fprintf(stdout, "s3:%08lX ",		(uint32_t)ex->regs->gpr[19]);
	fprintf(stdout, "s4:%08lX ",		(uint32_t)ex->regs->gpr[20]);
	fprintf(stdout, "s5:%08lX ",		(uint32_t)ex->regs->gpr[21]);
	fprintf(stdout, "s6:%08lX\n",		(uint32_t)ex->regs->gpr[22]);
	fprintf(stdout, "s7:%08lX ",		(uint32_t)ex->regs->gpr[23]);

	fprintf(stdout, "gp:%08lX ",		(uint32_t)ex->regs->gpr[28]);
	fprintf(stdout, "sp:%08lX ",		(uint32_t)ex->regs->gpr[29]);
	fprintf(stdout, "fp:%08lX ",		(uint32_t)ex->regs->gpr[30]);
	fprintf(stdout, "ra:%08lX \n",		(uint32_t)ex->regs->gpr[31]);
	fprintf(stdout, "lo:%016llX ",		ex->regs->lo);
	fprintf(stdout, "hi:%016llX\n",		ex->regs->hi);

	fprintf(stdout, "-------------------------------------------------FP Registers---");
	for (int i = 0; i<32; i++) {
		fprintf(stdout, "%02u:%016llX  ", i, ex->regs->fpr[i]);
		if ((i % 3) == 2) {
			fprintf(stdout, "\n");
		}
	}

	console_render();
	abort();
}

/**
 * @brief Fetch the string name of the exception
 *
 * @param[in] cr
 *            Cause register's value
 *
 * @return String representation of the exception
 */
static const char* __get_exception_name(exception_t *ex)
{
	static const char* exceptionMap[] =
	{
		"Interrupt",								// 0
		"TLB Modification",							// 1
		"TLB Miss (load/instruction fetch)",		// 2
		"TLB Miss (store)",							// 3
		"Address Error (load/instruction fetch)",	// 4
		"Address Error (store)",					// 5
		"Bus Error (instruction fetch)",			// 6
		"Bus Error (data reference: load/store)",	// 7
		"Syscall",									// 8
		"Breakpoint",								// 9
		"Reserved Instruction",						// 10
		"Coprocessor Unusable",						// 11
		"Arithmetic Overflow",						// 12
		"Trap",										// 13
		"Reserved",									// 13
		"Floating-Point",							// 15
		"Reserved",									// 16
		"Reserved",									// 17
		"Reserved",									// 18
		"Reserved",									// 19
		"Reserved",									// 20
		"Reserved",									// 21
		"Reserved",									// 22
		"Watch",									// 23
		"Reserved",									// 24
		"Reserved",									// 25
		"Reserved",									// 26
		"Reserved",									// 27
		"Reserved",									// 28
		"Reserved",									// 29
		"Reserved",									// 30
		"Reserved",									// 31
	};


	// When possible, by peeking into the exception state and COP0 registers
	// we can provide a more detailed exception name.
	uint32_t epc = ex->regs->epc + (ex->regs->cr & C0_CAUSE_BD ? 4 : 0);
	uint32_t badvaddr = C0_BADVADDR();

	switch (ex->code) {
	case EXCEPTION_CODE_FLOATING_POINT:
		if (ex->regs->fc31 & C1_CAUSE_DIV_BY_0) {
			return "Floating point divide by zero";
		} else if (ex->regs->fc31 & C1_CAUSE_INVALID_OP) {
			return "Floating point invalid operation";
		} else if (ex->regs->fc31 & C1_CAUSE_OVERFLOW) {
			return "Floating point overflow";
		} else if (ex->regs->fc31 & C1_CAUSE_UNDERFLOW) {
			return "Floating point underflow";
		} else if (ex->regs->fc31 & C1_CAUSE_INEXACT_OP) {
			return "Floating point inexact operation";
		} else {
			return "Generic floating point";
		}
	case EXCEPTION_CODE_TLB_LOAD_I_MISS:
		if (epc == badvaddr) {
			return "Invalid program counter address";
		} else if (badvaddr < 128) {
			// This is probably a NULL pointer dereference, though it can go through a structure or an array,
			// so leave some margin to the actual faulting address.
			return "NULL pointer dereference (read)";
		} else {
			return "Read from invalid memory address";
		}
	case EXCEPTION_CODE_TLB_STORE_MISS:
		if (badvaddr < 128) {
			return "NULL pointer dereference (write)";
		} else {
			return "Write to invalid memory address";
		}
	case EXCEPTION_CODE_TLB_MODIFICATION:
		return "Write to read-only memory";
	case EXCEPTION_CODE_LOAD_I_ADDRESS_ERROR:
		if (epc == badvaddr) {
			return "Misaligned program counter address";
		} else {
			return "Misaligned read from memory";
		}
	case EXCEPTION_CODE_STORE_ADDRESS_ERROR:
		return "Misaligned write to memory";
	case EXCEPTION_CODE_SYS_CALL:
		return "Unhandled syscall";

	default:
		return exceptionMap[ex->code];
	}
}

/**
 * @brief Fetch relevant registers
 *
 * @param[out] e
 *             Pointer to structure describing the exception
 * @param[in]  type
 *             Exception type.  Either #EXCEPTION_TYPE_CRITICAL or 
 *             #EXCEPTION_TYPE_RESET
 * @param[in]  regs
 *             CPU register status at exception time
 */
static void __fetch_regs(exception_t* e, int32_t type, reg_block_t *regs)
{
	e->regs = regs;
	e->type = type;
	e->code = C0_GET_CAUSE_EXC_CODE(e->regs->cr);
	e->info = __get_exception_name(e);
}

/**
 * @brief Respond to a critical exception
 */
void __onCriticalException(reg_block_t* regs)
{
	exception_t e;

	if(!__exception_handler) { return; }

	__fetch_regs(&e, EXCEPTION_TYPE_CRITICAL, regs);
	__exception_handler(&e);
}

/** @} */

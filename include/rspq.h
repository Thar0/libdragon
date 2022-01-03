/**
 * @file rspq.h
 * @brief RSP Command queue
 * @ingroup rsp
 * 
 * The RSP command queue library provides the basic infrastructure to allow
 * a very efficient use of the RSP coprocessor. On the CPU side, it implements
 * an API to enqueue "commands" to be executed by RSP into a ring buffer, that
 * is concurrently consumed by RSP in background. On the RSP side, it provides the
 * core loop that reads and execute the queue prepared by the CPU, and an
 * infrastructure to write "RSP overlays", that is libraries that plug upon
 * the RSP command queue to perform actual RSP jobs (eg: 3D graphics, audio, etc.).
 * 
 * The library is extremely efficient. It is designed for very high throughput
 * and low latency, as the RSP pulls by the queue concurrently as the CPU 
 * fills it. Through some complex synchronization paradigms, both CPU and RSP
 * run fully lockless, that is never need to explicitly synchronize with
 * each other (unless requested by the user). The CPU can keep filling the
 * queue and must only wait for RSP in case the queue becomes full; on the
 * other side, the RSP can keep processing the queue without ever talking to
 * the CPU.
 * 
 * The library has been designed to be able to enqueue thousands of RSP commands
 * per frame without its overhead to be measurable, which should be more than
 * enough for most use cases.
 * 
 * ## Commands
 * 
 * Each command in the queue is made by one or more 32-bit words (up to
 * #RSPQ_MAX_COMMAND_SIZE). The MSB of the first word is the command ID. The
 * higher 4 bits are called the "overlay ID" and identify the overlay that is
 * able to execute the command; the lower 4 bits are the command index, which
 * identify the command within the overlay. For instance, command ID 0x37 is
 * command index 7 in overlay 3.
 * 
 * As the RSP executes the queue, it will parse the command ID and dispatch
 * it for execution. When required, the RSP will automatically load the
 * RSP overlay needed to execute a command. In the previous example, the RSP
 * will load into IMEM/DMEM overlay 3 (unless it was already loaded) and then
 * dispatch command 7 to it.
 * 
 * ## Higher-level libraries
 * 
 * Higher-level libraries that come with their RSP ucode can be designed to
 * use the RSP command queue to efficiently coexist with all other RSP libraries
 * provided by libdragon. In fact, by using the overlay mechanism, each library
 * can register its own overlay ID, and enqueue commands to be executed by the
 * RSP through the same unique queue.
 * 
 * End-users can then use all these libraries at the same time, without having
 * to arrange for complex RSP synchronization, asynchronous execution or plan for
 * efficient context switching. In fact, they don't even need to be aware that
 * the libraries are using the RSP. Through the unified command queue,
 * the RSP can be used efficiently and effortlessly without idle time, nor
 * wasting CPU cycles waiting for completion of a task before switching to 
 * another one.
 * 
 * Higher-level libraries that are designed to use the RSP command queue must:
 * 
 *   * Call #rspq_init at initialization. The function can be called multiple
 *     times by different libraries, with no side-effect.
 *   * Call #rspq_overlay_register to register a #rsp_ucode_t as RSP command
 *     queue overlay, assigning an overlay ID to it.
 *   * Provide higher-level APIs that, when required, call #rspq_write_begin,
 *     #rspq_write_end and #rspq_flush to enqueue commands for the RSP. For
 *     instance, a matrix library might provide a "matrix_mult" function that
 *     internally calls #rspq_write_begin/#rspq_write_end to enqueue a command
 *     for the RSP to perform the calculation.
 * 
 * Normally, end-users will not need to manually enqueue commands in the RSP
 * queue: they should only call higher-level APIs which internally do that.
 * 
 * ## Blocks
 * 
 * A block (#rspq_block_t) is a prerecorded sequence of RSP commands that can
 * be played back. Blocks can be created via #rspq_block_begin / #rspq_block_end,
 * and then executed by #rspq_block_run. It is also possible to do nested
 * calls (a block can call another block), up to 8 levels deep.
 * 
 * A block is very efficient to run because it is played back by the RSP itself.
 * The CPU just enqueues a single command that "calls" the block. It is thus
 * much faster than enqueuing the same commands every frame.
 * 
 * Blocks can be used by higher-level libraries as an internal tool to efficiently
 * drive the RSP (without having to repeat common sequence of commands), or
 * they can be used by end-users to record and replay batch of commands, similar
 * to OpenGL 1.x display lists.
 * 
 * Notice that this library does not support static (compile-time) blocks.
 * Blocks must always be created at runtime once (eg: at init time) before
 * being used.
 * 
 * ## Syncpoints
 * 
 * The RSP command queue is designed to be fully lockless, but sometimes it is
 * required to know when the RSP has actually executed an enqueued command or
 * not (eg: to use its result). To do so, this library offers a synchronization
 * primitive called "syncpoint" (#rspq_syncpoint_t). A syncpoint can be
 * created via #rspq_syncpoint and records the current writing position in the
 * queue. It is then possible to call #rspq_check_syncpoint to check whether
 * the RSP has reached that position, or #rspq_wait_syncpoint to wait for
 * the RSP to reach that position.
 * 
 * Syncpoints are implemented using RSP interrupts, so their overhead is small
 * but still measurable. They should not be abused.
 * 
 * ## High-priority queue
 * 
 * This library offers a mechanism to preempt the execution of RSP to give
 * priority to very urgent tasks: the high-priority queue. Since the
 * moment a high-priority queue is created via #rspq_highpri_begin, the RSP
 * immediately suspends execution of the command queue, and switches to
 * the high-priority queue, waiting for commands. All commands added via
 * standard APIs (#rspq_write_begin / #rspq_write_end) are then directed
 * to the high-priority queue, until #rspq_highpri_end is called. Once the
 * RSP has finished executing all the commands enqueue in the high-priority
 * queue, it resumes execution of the standard queue.
 * 
 * The net effect is that commands enqueued in the high-priority queue are
 * executed right away by the RSP, irrespective to whatever was currently
 * enqueued in the standard queue. This can be useful for running tasks that
 * require immediate execution, like for instance audio processing.
 * 
 * If required, it is possible to call #rspq_highpri_sync to wait for the
 * high-priority queue to be fully executed.
 * 
 * Notice that the RSP cannot be fully preempted, so switching to the high-priority
 * queue can only happen after a command has finished execution (before starting
 * the following one). This can have an effect on latency if a single command
 * has a very long execution time; RSP overlays should in general prefer
 * providing smaller, faster commands.
 * 
 * This feature should normally not be used by end-users, but by libraries
 * in which a very low latency of RSP execution is paramount to their workings.
 * 
 */

#ifndef __LIBDRAGON_RSPQ_H
#define __LIBDRAGON_RSPQ_H

#include <stdint.h>
#include <rsp.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum size of a command (in 32-bit words). */
#define RSPQ_MAX_COMMAND_SIZE          16


/**
 * @brief A preconstructed block of commands
 * 
 * To improve performance of execution of sequences of commands, it is possible
 * to create a "block". A block is a fixed set of commands that is created
 * once and executed multiple times.
 * 
 * To create a block, use #rspq_block_begin and #rspq_block_end. After creation,
 * you can use #rspq_block_run at any point to run it. If you do not need the
 * block anymore, use #rspq_block_free to dispose it.
 */
typedef struct rspq_block_s rspq_block_t;

/**
 * @brief A syncpoint in the command list
 * 
 * A syncpoint can be thought of as a pointer to a position in the command list.
 * After creation, it is possible to later check whether the RSP has reached it
 * or not.
 * 
 * To create a syncpoint, use #rspq_syncpoint that returns a syncpoint that
 * references the current position. Call #rspq_check_syncpoint or #rspq_wait_syncpoint
 * to respectively do a single check or block waiting for the syncpoint to be
 * reached by RSP.
 * 
 * Syncpoints are implemented using interrupts, so they have a light but non
 * trivial overhead. Do not abuse them. For instance, it is reasonable to use
 * tens of syncpoints per frame, but not hundreds or thousands of them.
 * 
 * @note A valid syncpoint is an integer greater than 0.
 */
typedef int rspq_syncpoint_t;

/**
 * @brief Initialize the RSP command list.
 */
void rspq_init(void);

/**
 * @brief Shut down the RSP command list.
 */
void rspq_close(void);


/**
 * @brief Register a ucode overlay into the command list engine.
 * 
 * This function registers a ucode overlay into the command list engine.
 * An overlay is a ucode that has been written to be compatible with the
 * command list engine (see rsp_queue.inc) and is thus able to executed commands
 * that are enqueued in the command list.
 * 
 * Each command in the command list starts with a 8-bit ID, in which the
 * upper 4 bits are the overlay ID and the lower 4 bits are the command ID.
 * The ID specified with this function is the overlay ID to associated with
 * the ucode. For instance, calling this function with ID 0x3 means that 
 * the overlay will be associated with commands 0x30 - 0x3F. The overlay ID
 * 0 is reserved to the command list engine.
 * 
 * Notice that it is possible to call this function multiple times with the
 * same ucode in case the ucode exposes more than 16 commands. For instance,
 * an ucode that handles up to 32 commands could be registered twice with
 * IDs 0x6 and 0x7, so that the whole range 0x60-0x7F is assigned to it.
 * When calling multiple times, consecutive IDs must be used.
 *             
 * @param      overlay_ucode  The ucode to register
 * @param[in]  id             The overlay ID that will be associated to this ucode.
 */
void rspq_overlay_register(rsp_ucode_t *overlay_ucode, uint8_t id);

/**
 * @brief Return a pointer to the overlay state (in RDRAM)
 * 
 * Overlays can define a section of DMEM as persistent state. This area will be
 * preserved across overlay switching, by reading back into RDRAM the DMEM
 * contents when the overlay is switched away.
 * 
 * This function returns a pointer to the state area in RDRAM (not DMEM). It is
 * meant to modify the state on the CPU side while the overlay is not loaded.
 * The layout of the state and its size should be known to the caller.
 *
 * @param      overlay_ucode  The ucode overlay for which the state pointer will be returned.
 *
 * @return     Pointer to the overlay state (in RDRAM)
 */
void* rspq_overlay_get_state(rsp_ucode_t *overlay_ucode);

/**
 * @brief Begin writing a command to the current RSP command list.
 * 
 * This function must be called when a new command must be written to
 * the command list. It returns a pointer where the command can be written.
 * Call #rspq_write_end to terminate the command.
 * 
 * @return A pointer where the next command can be written.
 *
 * @code{.c}
 * 		// This example adds to the command list a sample command called
 * 		// CMD_SPRITE with code 0x3A (overlay 3, command A), with its arguments,
 * 		// for a total of three words.
 * 		
 * 		#define CMD_SPRITE  0x3A000000
 * 		
 * 		uint32_t *rspq = rspq_write_begin();
 * 		*rspq++ = CMD_SPRITE | sprite_num;
 * 		*rspq++ = (x0 << 16) | y0;
 * 		*rspq++ = (x1 << 16) | y1;
 * 		rspq_write_end(rspq);
 * @endcode
 *
 * @note Each command can be up to RSPQ_MAX_COMMAND_SIZE 32-bit words. Make
 * sure not to write more than that size without calling #rspq_write_end.
 * 
 * @hideinitializer
 */

// FOREACH helpers
#define __FE_0(_call, ...)
#define __FE_1(_call, x)       _call(x)
#define __FE_2(_call, x, ...)  _call(x) __FE_1(_call, __VA_ARGS__)
#define __FE_3(_call, x, ...)  _call(x) __FE_2(_call, __VA_ARGS__)
#define __FE_4(_call, x, ...)  _call(x) __FE_3(_call, __VA_ARGS__)
#define __FE_5(_call, x, ...)  _call(x) __FE_4(_call, __VA_ARGS__)
#define __FE_6(_call, x, ...)  _call(x) __FE_5(_call, __VA_ARGS__)
#define __FE_7(_call, x, ...)  _call(x) __FE_6(_call, __VA_ARGS__)
#define __FE_8(_call, x, ...)  _call(x) __FE_7(_call, __VA_ARGS__)
#define __FE_9(_call, x, ...)  _call(x) __FE_8(_call, __VA_ARGS__)
#define __FE_10(_call, x, ...) _call(x) __FE_9(_call, __VA_ARGS__)
#define __FE_11(_call, x, ...) _call(x) __FE_10(_call, __VA_ARGS__)
#define __FE_12(_call, x, ...) _call(x) __FE_11(_call, __VA_ARGS__)
#define __FE_13(_call, x, ...) _call(x) __FE_12(_call, __VA_ARGS__)
#define __FE_14(_call, x, ...) _call(x) __FE_13(_call, __VA_ARGS__)
#define __FE_15(_call, x, ...) _call(x) __FE_14(_call, __VA_ARGS__)
#define __FE_16(_call, x, ...) _call(x) __FE_15(_call, __VA_ARGS__)
#define __FE_17(_call, x, ...) _call(x) __FE_16(_call, __VA_ARGS__)
#define __FE_18(_call, x, ...) _call(x) __FE_17(_call, __VA_ARGS__)
#define __FE_19(_call, x, ...) _call(x) __FE_18(_call, __VA_ARGS__)
#define __FE_20(_call, x, ...) _call(x) __FE_19(_call, __VA_ARGS__)
#define __FE_21(_call, x, ...) _call(x) __FE_20(_call, __VA_ARGS__)
#define __FE_22(_call, x, ...) _call(x) __FE_21(_call, __VA_ARGS__)
#define __FE_23(_call, x, ...) _call(x) __FE_22(_call, __VA_ARGS__)
#define __FE_24(_call, x, ...) _call(x) __FE_23(_call, __VA_ARGS__)
#define __FE_25(_call, x, ...) _call(x) __FE_24(_call, __VA_ARGS__)
#define __FE_26(_call, x, ...) _call(x) __FE_25(_call, __VA_ARGS__)
#define __FE_27(_call, x, ...) _call(x) __FE_26(_call, __VA_ARGS__)
#define __FE_28(_call, x, ...) _call(x) __FE_27(_call, __VA_ARGS__)
#define __FE_29(_call, x, ...) _call(x) __FE_28(_call, __VA_ARGS__)
#define __FE_30(_call, x, ...) _call(x) __FE_29(_call, __VA_ARGS__)
#define __FE_31(_call, x, ...) _call(x) __FE_30(_call, __VA_ARGS__)

// Get the Nth variadic argument
#define __GET_NTH_ARG(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, N, ...)  N

// Return the number of variadic arguments
#define __COUNT_VARARGS(...)     __GET_NTH_ARG("ignored", ##__VA_ARGS__, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

// Return 1 if there is at least one variadic argument, otherwise 0
#define __HAS_VARARGS(...)       __GET_NTH_ARG("ignored", ##__VA_ARGS__, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0)

// Call macro fn for each variadic argument
#define __CALL_FOREACH(fn, ...)  __GET_NTH_ARG("ignored", ##__VA_ARGS__, __FE_31, __FE_30, __FE_29, __FE_28, __FE_27, __FE_26, __FE_25, __FE_24, __FE_23, __FE_22, __FE_21, __FE_20, __FE_19, __FE_18, __FE_17, __FE_16, __FE_15, __FE_14, __FE_13, __FE_12, __FE_11, __FE_10, __FE_9, __FE_8, __FE_7, __FE_6, __FE_5, __FE_4, __FE_3, __FE_2, __FE_1, __FE_0)(fn, ##__VA_ARGS__)

// Preprocessor token paste
#define __PPCAT2(n,x) n ## x
#define __PPCAT(n,x) __PPCAT2(n,x)

#define _rspq_write_prolog() \
    extern volatile uint32_t *rspq_cur_pointer, *rspq_cur_sentinel; \
    extern void rspq_next_buffer(void); \
    volatile uint32_t *ptr = rspq_cur_pointer+1; \
    (void)ptr;

#define _rspq_write_epilog() \
    if (rspq_cur_pointer > rspq_cur_sentinel) rspq_next_buffer();

#define _rspq_write_arg(arg) \
    *ptr++ = (arg);

#define _rspq_write0(cmd_id) ({ \
    _rspq_write_prolog(); \
    rspq_cur_pointer[0] = (cmd_id)<<24; \
    rspq_cur_pointer += 1; \
    _rspq_write_epilog(); \
})

#define _rspq_write1(cmd_id, arg0, ...) ({ \
    _rspq_write_prolog(); \
    __CALL_FOREACH(_rspq_write_arg, ##__VA_ARGS__); \
    rspq_cur_pointer[0] = ((cmd_id)<<24) | (arg0); \
    rspq_cur_pointer += 1 + __COUNT_VARARGS(__VA_ARGS__); \
    _rspq_write_epilog(); \
})

#define rspq_write(cmd_id, ...) \
    __PPCAT(_rspq_write, __HAS_VARARGS(__VA_ARGS__)) (cmd_id, ##__VA_ARGS__)


/**
 * @brief Make sure that RSP starts executing up to the last written command.
 * 
 * RSP processes the current command list asynchronously as it is being written.
 * If it catches up with the CPU, it halts itself and waits for the CPU to
 * notify that more commands are available. On the contrary, if the RSP lags
 * behind it might keep executing commands as they are written without ever
 * sleeping. So in general, at any given moment the RSP could be crunching
 * commands or sleeping waiting to be notified that more commands are available.
 * 
 * This means that writing a command (#rspq_write_begin / #rspq_write_end) is not
 * enough to make sure it is executed; depending on timing and batching performed
 * by RSP, it might either be executed automatically or not. #rspq_flush makes
 * sure that the RSP will see it and execute it.
 * 
 * This function does not block: it just make sure that the RSP will run the 
 * full command list written until now. If you need to actively wait until the
 * last written command has been executed, use #rspq_sync.
 * 
 * It is suggested to call rspq_flush every time a new "batch" of commands
 * has been written. In general, it is not a problem to call it often because
 * it is very very fast (takes only ~20 cycles). For instance, it can be called
 * after every rspq_write_end without many worries, but if you know that you are
 * going to write a number of subsequent commands in straight line code, you
 * can postpone the call to #rspq_flush after the whole sequence has been written.
 * 
 * @code{.c}
 * 		// This example shows some code configuring the lights for a scene.
 * 		// The command in this sample is called CMD_SET_LIGHT and requires
 * 		// a light index and the RGB colors for the list to update.
 *
 * 		#define CMD_SET_LIGHT  0x47000000
 *
 * 		for (int i=0; i<MAX_LIGHTS; i++) {
 * 			uint32_t *rspq = rspq_write_begin();
 * 			*rspq++ = CMD_SET_LIGHT | i;
 * 			*rspq++ = (lights[i].r << 16) | (lights[i].g << 8) | lights[i].b;
 * 			rspq_write_end(rspq);
 * 		}
 * 		
 * 		// After enqueuing multiple commands, it is sufficient
 * 		// to call rspq_flush once to make sure the RSP runs them (in case
 * 		// it was idling).
 * 		rspq_flush();
 * @endcode
 *
 * @note This is an experimental API. In the future, it might become 
 *       a no-op, and flushing could happen automatically at every rspq_write_end().
 *       We are keeping it separate from rspq_write_end() while experimenting more
 *       with the RSPQ API.
 * 
 * @note This function is a no-op if it is called while a block is being recorded
 *       (see #rspq_block_begin / #rspq_block_end). This means calling this function
 *       in a block recording context will not guarantee the execution of commands
 *       that were queued prior to starting the block.
 *       
 */
void rspq_flush(void);

/**
 * @brief Wait until all commands in command list have been executed by RSP.
 *
 * This function blocks until all commands present in the command list have
 * been executed by the RSP and the RSP is idle.
 * 
 * This function exists mostly for debugging purposes. Calling this function
 * is not necessary, as the CPU can continue enqueuing commands in the list
 * while the RSP is running them. If you need to synchronize between RSP and CPU
 * (eg: to access data that was processed by RSP) prefer using #rspq_syncpoint /
 * #rspq_wait_syncpoint which allows for more granular synchronization.
 */
#define rspq_sync() ({ \
    rspq_wait_syncpoint(rspq_syncpoint()); \
})

/**
 * @brief Create a syncpoint in the command list.
 * 
 * This function creates a new "syncpoint" referencing the current position
 * in the command list. It is possible to later check when the syncpoint
 * is reached by RSP via #rspq_check_syncpoint and #rspq_wait_syncpoint.
 *
 * @return     ID of the just-created syncpoint.
 * 
 * @note It is not possible to create a syncpoint within a block
 * 
 * @see #rspq_syncpoint_t
 */
rspq_syncpoint_t rspq_syncpoint(void);

/**
 * @brief Check whether a syncpoint was reached by RSP or not.
 * 
 * This function checks whether a syncpoint was reached. It never blocks.
 * If you need to wait for a syncpoint to be reached, use #rspq_wait_syncpoint
 * instead of polling this function.
 *
 * @param[in]  sync_id  ID of the syncpoint to check
 *
 * @return true if the RSP has reached the syncpoint, false otherwise
 * 
 * @see #rspq_syncpoint_t
 */
bool rspq_check_syncpoint(rspq_syncpoint_t sync_id);

/**
 * @brief Wait until a syncpoint is reached by RSP.
 * 
 * This function blocks waiting for the RSP to reach the specified syncpoint.
 * If the syncpoint was already called at the moment of call, the function
 * exits immediately.
 * 
 * @param[in]  sync_id  ID of the syncpoint to wait for
 * 
 * @see #rspq_syncpoint_t
 */
void rspq_wait_syncpoint(rspq_syncpoint_t sync_id);


/**
 * @brief Begin creating a new block.
 * 
 * This function initiates writing a command list block (see #rspq_block_t).
 * While a block is being written, all calls to #rspq_write_begin / #rspq_write_end
 * will record the commands into the block, without actually scheduling them for
 * execution. Use #rspq_block_end to close the block and get a reference to it.
 * 
 * Only one block at a time can be created. Calling #rspq_block_begin
 * twice (without any intervening #rspq_block_end) will cause an assert.
 *
 * During block creation, the RSP will keep running as usual and
 * execute commands that have been already enqueue in the command list.
 *       
 * @note Calls to #rspq_flush are ignored during block creation, as the RSP
 *       is not going to execute the block commands anyway.
 */
void rspq_block_begin(void);

/**
 * @brief Finish creating a block.
 * 
 * This function completes a block and returns a reference to it (see #rspq_block_t).
 * After this function is called, all subsequent #rspq_write_begin / #rspq_write_end
 * will resume working as usual: they will enqueue commands in the command list
 * for immediate RSP execution.
 * 
 * To run the created block, use #rspq_block_run. 
 *
 * @return A reference to the just created block
 * 
 * @see rspq_block_begin
 * @see rspq_block_run 
 */
rspq_block_t* rspq_block_end(void);

/**
 * @brief Add to the RSP command list a command that runs a block.
 * 
 * This function runs a block that was previously created via #rspq_block_begin
 * and #rspq_block_end. It schedules a special command in the command list
 * that will run the block, so that execution of the block will happen in
 * order relative to other commands in the command list.
 * 
 * Blocks can call other blocks. For instance, if a block A has been fully
 * created, it is possible to call `rspq_block_run(A)` at any point during the
 * creation of a second block B; this means that B will contain the special
 * command that will call A.
 *
 * @param block The block that must be run
 * 
 * @note The maximum depth of nested block calls is 8.
 */
void rspq_block_run(rspq_block_t *block);

/**
 * @brief Free a block that is not needed any more.
 * 
 * After calling this function, the block is invalid and must not be called
 * anymore.
 * 
 * @param  block  The block
 * 
 * @note If the block was being called by other blocks, these other blocks
 *       become invalid and will make the RSP crash if called. Make sure
 *       that freeing a block is only done when no other blocks reference it.
 */
void rspq_block_free(rspq_block_t *block);

/**
 * @brief Start building a high-priority queue.
 * 
 * This function enters a special mode in which a high-priority queue is
 * activated and can be filled with commands. After this command has been
 * called, all commands will be put in the high-priority queue, until
 * #rspq_highpri_end is called.
 * 
 * The RSP will start processing the high-priority queue almost instantly
 * (as soon as the current command is done), pausing the normal queue. This will
 * also happen while the high-priority queue is being built, to achieve the
 * lowest possible latency. When the RSP finishes processing the high priority
 * queue (after #rspq_highpri_end closes it), it resumes processing the normal
 * queue from the exact point that was left.
 * 
 * The goal of the high-priority queue is to either schedule latency-sensitive
 * commands like audio processing, or to schedule immediate RSP calculations
 * that should be performed right away, just like they were preempting what
 * the RSP is currently doing.
 * 
 * @note It is possible to create multiple high-priority queues by calling
 *       #rspq_highpri_begin / #rspq_highpri_end multiples time with short
 *       delays in-between. The RSP will process them in order. Notice that
 *       there is a overhead in doing so, so it might be advisable to keep
 *       the high-priority mode active for a longer period if possible. On the
 *       other hand, a shorter high-priority queue allows for the RSP to
 *       switch back to processing the normal queue before the next one
 *       is created.
 * 
 * @note It is not possible to create a block while the high-priority queue is
 *       active. Arrange for constructing blocks beforehand.
 *       
 * @note It is currently not possible to call a block from the
 *       high-priority queue. (FIXME: to be implemented)
 *       
 */
void rspq_highpri_begin(void);

/**
 * @brief Finish building the high-priority queue and close it.
 * 
 * This function terminates and closes the high-priority queue. After this
 * command is called, all following commands will be added to the normal queue.
 * 
 * Notice that the RSP does not wait for this function to be called: it will
 * start running the high-priority queue as soon as possible, even while it is
 * being built.
 */
void rspq_highpri_end(void);

/**
 * @brief Wait for the RSP to finish processing all high-priority queues.
 * 
 * This function will spin-lock waiting for the RSP to finish processing
 * all high-priority queues. It is meant for debugging purposes or for situations
 * in which the high-priority queue is known to be very short and fast to run,
 * so that the overhead of a syncpoint would be too high.
 * 
 * For longer/slower high-priority queues, it is advisable to use a #rspq_syncpoint_t
 * to synchronize (though it has a higher overhead).
 */
void rspq_highpri_sync(void);

/**
 * @brief Enqueue a no-op command in the queue.
 * 
 * This function enqueues a command that does nothing. This is mostly
 * useful for debugging purposes.
 */
void rspq_noop(void);

/**
 * @brief Enqueue a command that sets a signal in SP status
 * 
 * The SP status register has 8 bits called "signals" that can be
 * atomically set or cleared by both the CPU and the RSP. They can be used
 * to provide asynchronous communication.
 * 
 * This function allows to enqueue a command in the list that will set and/or
 * clear a combination of the above bits.
 * 
 * Notice that signal bits 2-7 are used by the command list engine itself, so this
 * function must only be used for bits 0 and 1.
 * 
 * @param[in]  signal  A signal set/clear mask created by composing SP_WSTATUS_* 
 *                     defines.
 *                     
 * @note This is an advanced function that should be used rarely. Most
 * synchronization requirements should be fulfilled via #rspq_syncpoint which is
 * easier to use.
 */
void rspq_signal(uint32_t signal);

/**
 * @brief Enqueue a command to do a DMA transfer from DMEM to RDRAM
 *
 * @param      rdram_addr  The RDRAM address (destination, must be aligned to 8)
 * @param[in]  dmem_addr   The DMEM address (source, must be aligned to 8)
 * @param[in]  len         Number of bytes to transfer (must be multiple of 8)
 * @param[in]  is_async    If true, the RSP does not wait for DMA completion
 *                         and processes the next command as the DMA is in progress.
 *                         If false, the RSP waits until the transfer is finished
 *                         before processing the next command.
 *                         
 * @note The argument is_async refers to the RSP only. From the CPU standpoint,
 *       this function is always asynchronous as it just enqueues a command
 *       in the list. 
 */
void rspq_dma_to_rdram(void *rdram_addr, uint32_t dmem_addr, uint32_t len, bool is_async);

/**
 * @brief Enqueue a command to do a DMA transfer from RDRAM to DMEM
 *
 * @param[in]  dmem_addr   The DMEM address (destination, must be aligned to 8)
 * @param      rdram_addr  The RDRAM address (source, must be aligned to 8)
 * @param[in]  len         Number of bytes to transfer (must be multiple of 8)
 * @param[in]  is_async    If true, the RSP does not wait for DMA completion
 *                         and processes the next command as the DMA is in progress.
 *                         If false, the RSP waits until the transfer is finished
 *                         before processing the next command.
 *                         
 * @note The argument is_async refers to the RSP only. From the CPU standpoint,
 *       this function is always asynchronous as it just enqueues a command
 *       in the list. 
 */
void rspq_dma_to_dmem(uint32_t dmem_addr, void *rdram_addr, uint32_t len, bool is_async);

#ifdef __cplusplus
}
#endif

#endif

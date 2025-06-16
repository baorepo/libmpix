/**
 * SPDX-License-Identifier: Apache-2.0
 * @defgroup mpix_operation mpix/op.h
 * @brief Implementing new types of operations
 * @{
 */
#ifndef MPIX_OPERATION_H
#define MPIX_OPERATION_H

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include <mpix/formats.h>
#include <mpix/port.h>
#include <mpix/ring.h>
#include <mpix/utils.h>

/**
 * @brief One step of a line operation pipeline
 *
 * @c mpix_base_op structs are chained together into a linked list.
 * Each step of the linked list contain a ring buffer for the input data, and a pointer to a
 * conversion function processing it.
 * Along with extra metadata, this is used to process data as a operation of lines.
 */
struct mpix_base_op {
	/** Linked-list entry */
	struct mpix_base_op *next;
	/** Name of the operation, useful for debugging the operation */
	const uint8_t *name;
	/** Pixel input format */
	uint32_t format_src;
	/** Pixel output format */
	uint32_t format_dst;
	/** Width of the image in pixels */
	uint16_t width;
	/** Height of the image in pixels */
	uint16_t height;
	/** Current position within the frame */
	uint16_t line_offset;
	/** Number of lines of context around the line being converted */
	uint16_t window_size;
	/** Number of bytes of input needed to call @c run() */
	uint16_t threshold;
	/** Whether the ring buffer memory is heap-allocated or external */
	bool is_heap;
	/** Ring buffer that keeps track of the position in bytes */
	struct mpix_ring ring;
	/** Function that performs the I/O */
	void (*run)(struct mpix_base_op *op);
	/** Timestamp since the op started working in CPU cycles */
	uint32_t start_time_us;
	/** Total time spent working in this op through the operation in CPU cycles */
	uint32_t total_time_us;
};

/**
 * @brief Get the pitch size of one line for an operation.
 *
 * @return The pitch value, or zero for variable pitch.
 */
static inline size_t mpix_op_pitch(struct mpix_base_op *op)
{
	return op->width * mpix_bits_per_pixel(op->format_src) / BITS_PER_BYTE;
}

/**
 * @brief Find an operation on a list given its input and output format.
 *
 * @param list An array of pointer to operations of any type.
 * @param format_src Four Character Code to match for the source format.
 * @param format_dst Four Character Code to match for the destination format.
 * @return A pointer to the operation element of the list, or NULL if none is found.
 */
static inline void *mpix_op_by_format(void *list, uint32_t format_src, uint32_t format_dst)
{
	struct mpix_base_op **op_list = list;

	for (size_t i = 0; op_list[i] != NULL; i++) {
		if (op_list[i]->format_src == format_src &&
		    op_list[i]->format_dst == format_dst) {
			return op_list[i];
		}
	}
	return NULL;
}

/**
 * @brief Request a pointer the next specified bytes of data from the input buffer
 *
 * This will shift the "read" position by as many bytes.
 * The buffer obtained contains the input data to process.
 * The lines will be considered as converted as soon as @ref mpix_op_done() is called,
 * which will feed the line into the next step of the operation.
 *
 * @param op Current operation in progress.
 * @param sz Number of bytes to request.
 * @return The requested buffer, never NULL.
 */
static inline uint8_t *mpix_op_get_input_bytes(struct mpix_base_op *op, size_t sz)
{
	uint8_t *data;

	data = mpix_ring_read(&op->ring, sz);
	assert(data != NULL /* Asked for more input bytes than available in the buffer */);

	return data;
}

/**
 * @brief Request a pointer the next specified bytes of data from the output buffer
 *
 * This will shift the "write" position by as many bytes.
 * The buffer obtained can be used to store the processed data.
 * The lines will be considered as converted as soon as @ref mpix_op_done() is called,
 * which will feed the line into the next step of the operation.
 *
 * @param op Current operation in progress.
 * @param sz Number of bytes to request.
 * @return The requested buffer, never NULL.
 */
static inline uint8_t *mpix_op_get_output_bytes(struct mpix_base_op *op, size_t sz)
{
	uint8_t *data;

	assert(op->next != NULL /* Missing operation at the end, cannot get output buffer */);

	data = mpix_ring_write(&op->next->ring, sz);
	assert(data != NULL /* Asked for more output bytes than available in the buffer */);

	return data;
}

/**
 * @brief Request a pointer the next bytes of input buffer.
 *
 * This will not shift the "read" position, but will shift the "peek" position.
 * The buffer obtained contains the input data to process.
 *
 * @param op Current operation in progress.
 * @param sz Number of bytes to request.
 * @return The requested buffer, never NULL.
 */
static inline uint8_t *mpix_op_peek_input_bytes(struct mpix_base_op *op, size_t sz)
{
	uint8_t *data;

	data = mpix_ring_peek(&op->ring, sz);
	assert(data != NULL /* Asked for more input bytes than peekable in the buffer */);

	return data;
}

/**
 * @brief Request a pointer to the next line of data .
 *
 * This permits to implement a lookahead operation when one or several lines of context is needed
 * in addition to the line converted.
 *
 * @return The pointer to the input data.
 */
static inline uint8_t *mpix_op_get_output_line(struct mpix_base_op *op)
{
	return mpix_op_get_output_bytes(op, mpix_op_pitch(op->next));
}

/**
 * @brief Request a pointer to the next line of data taking it out of the input stream.
 *
 * This permits to step one line forward through the stream as one line of data is requested.
 * in addition to the line converted.
 *
 * @return The pointer to the input data.
 */
static inline const uint8_t *mpix_op_get_input_line(struct mpix_base_op *op)
{
	op->line_offset++;
	return mpix_op_get_input_bytes(op, mpix_op_pitch(op));
}

/**
 * @brief Request a pointer to the next line of data without affecting the input stream.
 *
 * This permits to implement a lookahead operation when one or several lines of context is needed
 * in addition to the line converted.
 *
 * @return The pointer to the input data.
 */
static inline uint8_t *mpix_op_peek_input_line(struct mpix_base_op *op)
{
	return mpix_op_peek_input_bytes(op, mpix_op_pitch(op));
}

/**
 * @brief Request a pointer to the entire input buffer content, consumed from the input operation.
 *
 * @param op Current operation in progress.
 * @return The pointer to the input data.
 */
static inline const uint8_t *mpix_op_get_all_input(struct mpix_base_op *op, size_t *sz)
{
	*sz = mpix_ring_tailroom(&op->ring);
	return mpix_ring_read(&op->ring, *sz);
}

/**
 * @brief Request a pointer the next bytes of output buffer.
 *
 * This will not shift the "write" position.
 * The buffer obtained can receive the output data, and the number of bytes written need to be
 * confirmed using @ref mpix_op_get_output_bytes().
 *
 * @param op Current operation in progress.
 * @param sz Pointer filled with the number of bytes available.
 * @return The requested buffer, never NULL.
 */
static inline uint8_t *mpix_op_peek_output(struct mpix_base_op *op, size_t *sz)
{
	*sz = mpix_ring_tailroom(&op->ring);
	return op->ring.data + op->ring.head;
}

static inline void mpix_op_run(struct mpix_base_op *op)
{
	if (op != NULL && op->run != NULL) {
		/* Start the counter of the next operation */
		op->start_time_us = mpix_port_get_uptime_us();

		while (mpix_ring_total_used(&op->ring) >= op->threshold &&
		       op->line_offset < op->height) {
			op->run(op);
		}
	}
}

/**
 * @brief Mark the line obtained with @ref mpix_op_get_output_line as converted.
 *
 * This will let the next step of the operation know that a new line was converted.
 * This allows the pipeline to trigger the next step if there is enough data submitted to it.
 *
 * @param op Current operation in progress.
 */
static inline void mpix_op_done(struct mpix_base_op *op)
{
	uint32_t done_time_us = mpix_port_get_uptime_us();

	/* Flush the timestamp to the counter */
	op->total_time_us += (op->start_time_us == 0) ? (0) : (done_time_us - op->start_time_us);

	/* Run the next operation in the chain now that more data is available */
	mpix_op_run(op->next);

	/* Resuming to this operation, reset the time counter */
	op->start_time_us = mpix_port_get_uptime_us();
}

#endif /** @} */

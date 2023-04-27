// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022 Facebook */

#include <errno.h>
#include <string.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "bpf_misc.h"

char _license[] SEC("license") = "GPL";

struct test_info {
	int x;
	struct bpf_dynptr ptr;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct bpf_dynptr);
} array_map1 SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct test_info);
} array_map2 SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} array_map3 SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} array_map4 SEC(".maps");

struct sample {
	int pid;
	long value;
	char comm[16];
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 4096);
} ringbuf SEC(".maps");

int err, val;

static int get_map_val_dynptr(struct bpf_dynptr *ptr)
{
	__u32 key = 0, *map_val;

	bpf_map_update_elem(&array_map3, &key, &val, 0);

	map_val = bpf_map_lookup_elem(&array_map3, &key);
	if (!map_val)
		return -ENOENT;

	bpf_dynptr_from_mem(map_val, sizeof(*map_val), 0, ptr);

	return 0;
}

/* Every bpf_ringbuf_reserve_dynptr call must have a corresponding
 * bpf_ringbuf_submit/discard_dynptr call
 */
SEC("?raw_tp")
__failure __msg("Unreleased reference id=2")
int ringbuf_missing_release1(void *ctx)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, val, 0, &ptr);

	/* missing a call to bpf_ringbuf_discard/submit_dynptr */

	return 0;
}

SEC("?raw_tp")
__failure __msg("Unreleased reference id=4")
int ringbuf_missing_release2(void *ctx)
{
	struct bpf_dynptr ptr1, ptr2;
	struct sample *sample;

	bpf_ringbuf_reserve_dynptr(&ringbuf, sizeof(*sample), 0, &ptr1);
	bpf_ringbuf_reserve_dynptr(&ringbuf, sizeof(*sample), 0, &ptr2);

	sample = bpf_dynptr_data(&ptr1, 0, sizeof(*sample));
	if (!sample) {
		bpf_ringbuf_discard_dynptr(&ptr1, 0);
		bpf_ringbuf_discard_dynptr(&ptr2, 0);
		return 0;
	}

	bpf_ringbuf_submit_dynptr(&ptr1, 0);

	/* missing a call to bpf_ringbuf_discard/submit_dynptr on ptr2 */

	return 0;
}

static int missing_release_callback_fn(__u32 index, void *data)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, val, 0, &ptr);

	/* missing a call to bpf_ringbuf_discard/submit_dynptr */

	return 0;
}

/* Any dynptr initialized within a callback must have bpf_dynptr_put called */
SEC("?raw_tp")
__failure __msg("Unreleased reference id")
int ringbuf_missing_release_callback(void *ctx)
{
	bpf_loop(10, missing_release_callback_fn, NULL, 0);
	return 0;
}

/* Can't call bpf_ringbuf_submit/discard_dynptr on a non-initialized dynptr */
SEC("?raw_tp")
__failure __msg("arg 1 is an unacquired reference")
int ringbuf_release_uninit_dynptr(void *ctx)
{
	struct bpf_dynptr ptr;

	/* this should fail */
	bpf_ringbuf_submit_dynptr(&ptr, 0);

	return 0;
}

/* A dynptr can't be used after it has been invalidated */
SEC("?raw_tp")
__failure __msg("Expected an initialized dynptr as arg #3")
int use_after_invalid(void *ctx)
{
	struct bpf_dynptr ptr;
	char read_data[64];

	bpf_ringbuf_reserve_dynptr(&ringbuf, sizeof(read_data), 0, &ptr);

	bpf_dynptr_read(read_data, sizeof(read_data), &ptr, 0, 0);

	bpf_ringbuf_submit_dynptr(&ptr, 0);

	/* this should fail */
	bpf_dynptr_read(read_data, sizeof(read_data), &ptr, 0, 0);

	return 0;
}

/* Can't call non-dynptr ringbuf APIs on a dynptr ringbuf sample */
SEC("?raw_tp")
__failure __msg("type=mem expected=ringbuf_mem")
int ringbuf_invalid_api(void *ctx)
{
	struct bpf_dynptr ptr;
	struct sample *sample;

	bpf_ringbuf_reserve_dynptr(&ringbuf, sizeof(*sample), 0, &ptr);
	sample = bpf_dynptr_data(&ptr, 0, sizeof(*sample));
	if (!sample)
		goto done;

	sample->pid = 123;

	/* invalid API use. need to use dynptr API to submit/discard */
	bpf_ringbuf_submit(sample, 0);

done:
	bpf_ringbuf_discard_dynptr(&ptr, 0);
	return 0;
}

/* Can't add a dynptr to a map */
SEC("?raw_tp")
__failure __msg("invalid indirect read from stack")
int add_dynptr_to_map1(void *ctx)
{
	struct bpf_dynptr ptr;
	int key = 0;

	bpf_ringbuf_reserve_dynptr(&ringbuf, val, 0, &ptr);

	/* this should fail */
	bpf_map_update_elem(&array_map1, &key, &ptr, 0);

	bpf_ringbuf_submit_dynptr(&ptr, 0);

	return 0;
}

/* Can't add a struct with an embedded dynptr to a map */
SEC("?raw_tp")
__failure __msg("invalid indirect read from stack")
int add_dynptr_to_map2(void *ctx)
{
	struct test_info x;
	int key = 0;

	bpf_ringbuf_reserve_dynptr(&ringbuf, val, 0, &x.ptr);

	/* this should fail */
	bpf_map_update_elem(&array_map2, &key, &x, 0);

	bpf_ringbuf_submit_dynptr(&x.ptr, 0);

	return 0;
}

/* A data slice can't be accessed out of bounds */
SEC("?raw_tp")
__failure __msg("value is outside of the allowed memory range")
int data_slice_out_of_bounds_ringbuf(void *ctx)
{
	struct bpf_dynptr ptr;
	void *data;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 8, 0, &ptr);

	data  = bpf_dynptr_data(&ptr, 0, 8);
	if (!data)
		goto done;

	/* can't index out of bounds of the data slice */
	val = *((char *)data + 8);

done:
	bpf_ringbuf_submit_dynptr(&ptr, 0);
	return 0;
}

SEC("?raw_tp")
__failure __msg("value is outside of the allowed memory range")
int data_slice_out_of_bounds_map_value(void *ctx)
{
	__u32 key = 0, map_val;
	struct bpf_dynptr ptr;
	void *data;

	get_map_val_dynptr(&ptr);

	data  = bpf_dynptr_data(&ptr, 0, sizeof(map_val));
	if (!data)
		return 0;

	/* can't index out of bounds of the data slice */
	val = *((char *)data + (sizeof(map_val) + 1));

	return 0;
}

/* A data slice can't be used after it has been released */
SEC("?raw_tp")
__failure __msg("invalid mem access 'scalar'")
int data_slice_use_after_release1(void *ctx)
{
	struct bpf_dynptr ptr;
	struct sample *sample;

	bpf_ringbuf_reserve_dynptr(&ringbuf, sizeof(*sample), 0, &ptr);
	sample = bpf_dynptr_data(&ptr, 0, sizeof(*sample));
	if (!sample)
		goto done;

	sample->pid = 123;

	bpf_ringbuf_submit_dynptr(&ptr, 0);

	/* this should fail */
	val = sample->pid;

	return 0;

done:
	bpf_ringbuf_discard_dynptr(&ptr, 0);
	return 0;
}

/* A data slice can't be used after it has been released.
 *
 * This tests the case where the data slice tracks a dynptr (ptr2)
 * that is at a non-zero offset from the frame pointer (ptr1 is at fp,
 * ptr2 is at fp - 16).
 */
SEC("?raw_tp")
__failure __msg("invalid mem access 'scalar'")
int data_slice_use_after_release2(void *ctx)
{
	struct bpf_dynptr ptr1, ptr2;
	struct sample *sample;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &ptr1);
	bpf_ringbuf_reserve_dynptr(&ringbuf, sizeof(*sample), 0, &ptr2);

	sample = bpf_dynptr_data(&ptr2, 0, sizeof(*sample));
	if (!sample)
		goto done;

	sample->pid = 23;

	bpf_ringbuf_submit_dynptr(&ptr2, 0);

	/* this should fail */
	sample->pid = 23;

	bpf_ringbuf_submit_dynptr(&ptr1, 0);

	return 0;

done:
	bpf_ringbuf_discard_dynptr(&ptr2, 0);
	bpf_ringbuf_discard_dynptr(&ptr1, 0);
	return 0;
}

/* A data slice must be first checked for NULL */
SEC("?raw_tp")
__failure __msg("invalid mem access 'mem_or_null'")
int data_slice_missing_null_check1(void *ctx)
{
	struct bpf_dynptr ptr;
	void *data;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 8, 0, &ptr);

	data  = bpf_dynptr_data(&ptr, 0, 8);

	/* missing if (!data) check */

	/* this should fail */
	*(__u8 *)data = 3;

	bpf_ringbuf_submit_dynptr(&ptr, 0);
	return 0;
}

/* A data slice can't be dereferenced if it wasn't checked for null */
SEC("?raw_tp")
__failure __msg("invalid mem access 'mem_or_null'")
int data_slice_missing_null_check2(void *ctx)
{
	struct bpf_dynptr ptr;
	__u64 *data1, *data2;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 16, 0, &ptr);

	data1 = bpf_dynptr_data(&ptr, 0, 8);
	data2 = bpf_dynptr_data(&ptr, 0, 8);
	if (data1)
		/* this should fail */
		*data2 = 3;

done:
	bpf_ringbuf_discard_dynptr(&ptr, 0);
	return 0;
}

/* Can't pass in a dynptr as an arg to a helper function that doesn't take in a
 * dynptr argument
 */
SEC("?raw_tp")
__failure __msg("invalid indirect read from stack")
int invalid_helper1(void *ctx)
{
	struct bpf_dynptr ptr;

	get_map_val_dynptr(&ptr);

	/* this should fail */
	bpf_strncmp((const char *)&ptr, sizeof(ptr), "hello!");

	return 0;
}

/* A dynptr can't be passed into a helper function at a non-zero offset */
SEC("?raw_tp")
__failure __msg("cannot pass in dynptr at an offset=-8")
int invalid_helper2(void *ctx)
{
	struct bpf_dynptr ptr;
	char read_data[64];

	get_map_val_dynptr(&ptr);

	/* this should fail */
	bpf_dynptr_read(read_data, sizeof(read_data), (void *)&ptr + 8, 0, 0);

	return 0;
}

/* A bpf_dynptr is invalidated if it's been written into */
SEC("?raw_tp")
__failure __msg("Expected an initialized dynptr as arg #1")
int invalid_write1(void *ctx)
{
	struct bpf_dynptr ptr;
	void *data;
	__u8 x = 0;

	get_map_val_dynptr(&ptr);

	memcpy(&ptr, &x, sizeof(x));

	/* this should fail */
	data = bpf_dynptr_data(&ptr, 0, 1);

	return 0;
}

/*
 * A bpf_dynptr can't be used as a dynptr if it has been written into at a fixed
 * offset
 */
SEC("?raw_tp")
__failure __msg("cannot overwrite referenced dynptr")
int invalid_write2(void *ctx)
{
	struct bpf_dynptr ptr;
	char read_data[64];
	__u8 x = 0;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &ptr);

	memcpy((void *)&ptr + 8, &x, sizeof(x));

	/* this should fail */
	bpf_dynptr_read(read_data, sizeof(read_data), &ptr, 0, 0);

	bpf_ringbuf_submit_dynptr(&ptr, 0);

	return 0;
}

/*
 * A bpf_dynptr can't be used as a dynptr if it has been written into at a
 * non-const offset
 */
SEC("?raw_tp")
__failure __msg("cannot overwrite referenced dynptr")
int invalid_write3(void *ctx)
{
	struct bpf_dynptr ptr;
	char stack_buf[16];
	unsigned long len;
	__u8 x = 0;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 8, 0, &ptr);

	memcpy(stack_buf, &val, sizeof(val));
	len = stack_buf[0] & 0xf;

	memcpy((void *)&ptr + len, &x, sizeof(x));

	/* this should fail */
	bpf_ringbuf_submit_dynptr(&ptr, 0);

	return 0;
}

static int invalid_write4_callback(__u32 index, void *data)
{
	*(__u32 *)data = 123;

	return 0;
}

/* If the dynptr is written into in a callback function, it should
 * be invalidated as a dynptr
 */
SEC("?raw_tp")
__failure __msg("cannot overwrite referenced dynptr")
int invalid_write4(void *ctx)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &ptr);

	bpf_loop(10, invalid_write4_callback, &ptr, 0);

	/* this should fail */
	bpf_ringbuf_submit_dynptr(&ptr, 0);

	return 0;
}

/* A globally-defined bpf_dynptr can't be used (it must reside as a stack frame) */
struct bpf_dynptr global_dynptr;

SEC("?raw_tp")
__failure __msg("type=map_value expected=fp")
int global(void *ctx)
{
	/* this should fail */
	bpf_ringbuf_reserve_dynptr(&ringbuf, 16, 0, &global_dynptr);

	bpf_ringbuf_discard_dynptr(&global_dynptr, 0);

	return 0;
}

/* A direct read should fail */
SEC("?raw_tp")
__failure __msg("invalid read from stack")
int invalid_read1(void *ctx)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &ptr);

	/* this should fail */
	val = *(int *)&ptr;

	bpf_ringbuf_discard_dynptr(&ptr, 0);

	return 0;
}

/* A direct read at an offset should fail */
SEC("?raw_tp")
__failure __msg("cannot pass in dynptr at an offset")
int invalid_read2(void *ctx)
{
	struct bpf_dynptr ptr;
	char read_data[64];

	get_map_val_dynptr(&ptr);

	/* this should fail */
	bpf_dynptr_read(read_data, sizeof(read_data), (void *)&ptr + 1, 0, 0);

	return 0;
}

/* A direct read at an offset into the lower stack slot should fail */
SEC("?raw_tp")
__failure __msg("invalid read from stack")
int invalid_read3(void *ctx)
{
	struct bpf_dynptr ptr1, ptr2;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 16, 0, &ptr1);
	bpf_ringbuf_reserve_dynptr(&ringbuf, 16, 0, &ptr2);

	/* this should fail */
	memcpy(&val, (void *)&ptr1 + 8, sizeof(val));

	bpf_ringbuf_discard_dynptr(&ptr1, 0);
	bpf_ringbuf_discard_dynptr(&ptr2, 0);

	return 0;
}

static int invalid_read4_callback(__u32 index, void *data)
{
	/* this should fail */
	val = *(__u32 *)data;

	return 0;
}

/* A direct read within a callback function should fail */
SEC("?raw_tp")
__failure __msg("invalid read from stack")
int invalid_read4(void *ctx)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &ptr);

	bpf_loop(10, invalid_read4_callback, &ptr, 0);

	bpf_ringbuf_submit_dynptr(&ptr, 0);

	return 0;
}

/* Initializing a dynptr on an offset should fail */
SEC("?raw_tp")
__failure __msg("cannot pass in dynptr at an offset=0")
int invalid_offset(void *ctx)
{
	struct bpf_dynptr ptr;

	/* this should fail */
	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &ptr + 1);

	bpf_ringbuf_discard_dynptr(&ptr, 0);

	return 0;
}

/* Can't release a dynptr twice */
SEC("?raw_tp")
__failure __msg("arg 1 is an unacquired reference")
int release_twice(void *ctx)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 16, 0, &ptr);

	bpf_ringbuf_discard_dynptr(&ptr, 0);

	/* this second release should fail */
	bpf_ringbuf_discard_dynptr(&ptr, 0);

	return 0;
}

static int release_twice_callback_fn(__u32 index, void *data)
{
	/* this should fail */
	bpf_ringbuf_discard_dynptr(data, 0);

	return 0;
}

/* Test that releasing a dynptr twice, where one of the releases happens
 * within a callback function, fails
 */
SEC("?raw_tp")
__failure __msg("arg 1 is an unacquired reference")
int release_twice_callback(void *ctx)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 32, 0, &ptr);

	bpf_ringbuf_discard_dynptr(&ptr, 0);

	bpf_loop(10, release_twice_callback_fn, &ptr, 0);

	return 0;
}

/* Reject unsupported local mem types for dynptr_from_mem API */
SEC("?raw_tp")
__failure __msg("Unsupported reg type fp for bpf_dynptr_from_mem data")
int dynptr_from_mem_invalid_api(void *ctx)
{
	struct bpf_dynptr ptr;
	int x = 0;

	/* this should fail */
	bpf_dynptr_from_mem(&x, sizeof(x), 0, &ptr);

	return 0;
}

SEC("?tc")
__failure __msg("cannot overwrite referenced dynptr") __log_level(2)
int dynptr_pruning_overwrite(struct __sk_buff *ctx)
{
	asm volatile (
		"r9 = 0xeB9F;				\
		 r6 = %[ringbuf] ll;			\
		 r1 = r6;				\
		 r2 = 8;				\
		 r3 = 0;				\
		 r4 = r10;				\
		 r4 += -16;				\
		 call %[bpf_ringbuf_reserve_dynptr];	\
		 if r0 == 0 goto pjmp1;			\
		 goto pjmp2;				\
	pjmp1:						\
		 *(u64 *)(r10 - 16) = r9;		\
	pjmp2:						\
		 r1 = r10;				\
		 r1 += -16;				\
		 r2 = 0;				\
		 call %[bpf_ringbuf_discard_dynptr];	"
		:
		: __imm(bpf_ringbuf_reserve_dynptr),
		  __imm(bpf_ringbuf_discard_dynptr),
		  __imm_addr(ringbuf)
		: __clobber_all
	);
	return 0;
}

SEC("?tc")
__success __msg("12: safe") __log_level(2)
int dynptr_pruning_stacksafe(struct __sk_buff *ctx)
{
	asm volatile (
		"r9 = 0xeB9F;				\
		 r6 = %[ringbuf] ll;			\
		 r1 = r6;				\
		 r2 = 8;				\
		 r3 = 0;				\
		 r4 = r10;				\
		 r4 += -16;				\
		 call %[bpf_ringbuf_reserve_dynptr];	\
		 if r0 == 0 goto stjmp1;		\
		 goto stjmp2;				\
	stjmp1:						\
		 r9 = r9;				\
	stjmp2:						\
		 r1 = r10;				\
		 r1 += -16;				\
		 r2 = 0;				\
		 call %[bpf_ringbuf_discard_dynptr];	"
		:
		: __imm(bpf_ringbuf_reserve_dynptr),
		  __imm(bpf_ringbuf_discard_dynptr),
		  __imm_addr(ringbuf)
		: __clobber_all
	);
	return 0;
}

SEC("?tc")
__failure __msg("cannot overwrite referenced dynptr") __log_level(2)
int dynptr_pruning_type_confusion(struct __sk_buff *ctx)
{
	asm volatile (
		"r6 = %[array_map4] ll;			\
		 r7 = %[ringbuf] ll;			\
		 r1 = r6;				\
		 r2 = r10;				\
		 r2 += -8;				\
		 r9 = 0;				\
		 *(u64 *)(r2 + 0) = r9;			\
		 r3 = r10;				\
		 r3 += -24;				\
		 r9 = 0xeB9FeB9F;			\
		 *(u64 *)(r10 - 16) = r9;		\
		 *(u64 *)(r10 - 24) = r9;		\
		 r9 = 0;				\
		 r4 = 0;				\
		 r8 = r2;				\
		 call %[bpf_map_update_elem];		\
		 r1 = r6;				\
		 r2 = r8;				\
		 call %[bpf_map_lookup_elem];		\
		 if r0 != 0 goto tjmp1;			\
		 exit;					\
	tjmp1:						\
		 r8 = r0;				\
		 r1 = r7;				\
		 r2 = 8;				\
		 r3 = 0;				\
		 r4 = r10;				\
		 r4 += -16;				\
		 r0 = *(u64 *)(r0 + 0);			\
		 call %[bpf_ringbuf_reserve_dynptr];	\
		 if r0 == 0 goto tjmp2;			\
		 r8 = r8;				\
		 r8 = r8;				\
		 r8 = r8;				\
		 r8 = r8;				\
		 r8 = r8;				\
		 r8 = r8;				\
		 r8 = r8;				\
		 goto tjmp3;				\
	tjmp2:						\
		 *(u64 *)(r10 - 8) = r9;		\
		 *(u64 *)(r10 - 16) = r9;		\
		 r1 = r8;				\
		 r1 += 8;				\
		 r2 = 0;				\
		 r3 = 0;				\
		 r4 = r10;				\
		 r4 += -16;				\
		 call %[bpf_dynptr_from_mem];		\
	tjmp3:						\
		 r1 = r10;				\
		 r1 += -16;				\
		 r2 = 0;				\
		 call %[bpf_ringbuf_discard_dynptr];	"
		:
		: __imm(bpf_map_update_elem),
		  __imm(bpf_map_lookup_elem),
		  __imm(bpf_ringbuf_reserve_dynptr),
		  __imm(bpf_dynptr_from_mem),
		  __imm(bpf_ringbuf_discard_dynptr),
		  __imm_addr(array_map4),
		  __imm_addr(ringbuf)
		: __clobber_all
	);
	return 0;
}

SEC("?tc")
__failure __msg("dynptr has to be at a constant offset") __log_level(2)
int dynptr_var_off_overwrite(struct __sk_buff *ctx)
{
	asm volatile (
		"r9 = 16;				\
		 *(u32 *)(r10 - 4) = r9;		\
		 r8 = *(u32 *)(r10 - 4);		\
		 if r8 >= 0 goto vjmp1;			\
		 r0 = 1;				\
		 exit;					\
	vjmp1:						\
		 if r8 <= 16 goto vjmp2;		\
		 r0 = 1;				\
		 exit;					\
	vjmp2:						\
		 r8 &= 16;				\
		 r1 = %[ringbuf] ll;			\
		 r2 = 8;				\
		 r3 = 0;				\
		 r4 = r10;				\
		 r4 += -32;				\
		 r4 += r8;				\
		 call %[bpf_ringbuf_reserve_dynptr];	\
		 r9 = 0xeB9F;				\
		 *(u64 *)(r10 - 16) = r9;		\
		 r1 = r10;				\
		 r1 += -32;				\
		 r1 += r8;				\
		 r2 = 0;				\
		 call %[bpf_ringbuf_discard_dynptr];	"
		:
		: __imm(bpf_ringbuf_reserve_dynptr),
		  __imm(bpf_ringbuf_discard_dynptr),
		  __imm_addr(ringbuf)
		: __clobber_all
	);
	return 0;
}

SEC("?tc")
__failure __msg("cannot overwrite referenced dynptr") __log_level(2)
int dynptr_partial_slot_invalidate(struct __sk_buff *ctx)
{
	asm volatile (
		"r6 = %[ringbuf] ll;			\
		 r7 = %[array_map4] ll;			\
		 r1 = r7;				\
		 r2 = r10;				\
		 r2 += -8;				\
		 r9 = 0;				\
		 *(u64 *)(r2 + 0) = r9;			\
		 r3 = r2;				\
		 r4 = 0;				\
		 r8 = r2;				\
		 call %[bpf_map_update_elem];		\
		 r1 = r7;				\
		 r2 = r8;				\
		 call %[bpf_map_lookup_elem];		\
		 if r0 != 0 goto sjmp1;			\
		 exit;					\
	sjmp1:						\
		 r7 = r0;				\
		 r1 = r6;				\
		 r2 = 8;				\
		 r3 = 0;				\
		 r4 = r10;				\
		 r4 += -24;				\
		 call %[bpf_ringbuf_reserve_dynptr];	\
		 *(u64 *)(r10 - 16) = r9;		\
		 r1 = r7;				\
		 r2 = 8;				\
		 r3 = 0;				\
		 r4 = r10;				\
		 r4 += -16;				\
		 call %[bpf_dynptr_from_mem];		\
		 r1 = r10;				\
		 r1 += -512;				\
		 r2 = 488;				\
		 r3 = r10;				\
		 r3 += -24;				\
		 r4 = 0;				\
		 r5 = 0;				\
		 call %[bpf_dynptr_read];		\
		 r8 = 1;				\
		 if r0 != 0 goto sjmp2;			\
		 r8 = 0;				\
	sjmp2:						\
		 r1 = r10;				\
		 r1 += -24;				\
		 r2 = 0;				\
		 call %[bpf_ringbuf_discard_dynptr];	"
		:
		: __imm(bpf_map_update_elem),
		  __imm(bpf_map_lookup_elem),
		  __imm(bpf_ringbuf_reserve_dynptr),
		  __imm(bpf_ringbuf_discard_dynptr),
		  __imm(bpf_dynptr_from_mem),
		  __imm(bpf_dynptr_read),
		  __imm_addr(ringbuf),
		  __imm_addr(array_map4)
		: __clobber_all
	);
	return 0;
}

/* Test that it is allowed to overwrite unreferenced dynptr. */
SEC("?raw_tp")
__success
int dynptr_overwrite_unref(void *ctx)
{
	struct bpf_dynptr ptr;

	if (get_map_val_dynptr(&ptr))
		return 0;
	if (get_map_val_dynptr(&ptr))
		return 0;
	if (get_map_val_dynptr(&ptr))
		return 0;

	return 0;
}

/* Test that slices are invalidated on reinitializing a dynptr. */
SEC("?raw_tp")
__failure __msg("invalid mem access 'scalar'")
int dynptr_invalidate_slice_reinit(void *ctx)
{
	struct bpf_dynptr ptr;
	__u8 *p;

	if (get_map_val_dynptr(&ptr))
		return 0;
	p = bpf_dynptr_data(&ptr, 0, 1);
	if (!p)
		return 0;
	if (get_map_val_dynptr(&ptr))
		return 0;
	/* this should fail */
	return *p;
}

/* Invalidation of dynptr slices on destruction of dynptr should not miss
 * mem_or_null pointers.
 */
SEC("?raw_tp")
__failure __msg("R1 type=scalar expected=percpu_ptr_")
int dynptr_invalidate_slice_or_null(void *ctx)
{
	struct bpf_dynptr ptr;
	__u8 *p;

	if (get_map_val_dynptr(&ptr))
		return 0;

	p = bpf_dynptr_data(&ptr, 0, 1);
	*(__u8 *)&ptr = 0;
	/* this should fail */
	bpf_this_cpu_ptr(p);
	return 0;
}

/* Destruction of dynptr should also any slices obtained from it */
SEC("?raw_tp")
__failure __msg("R7 invalid mem access 'scalar'")
int dynptr_invalidate_slice_failure(void *ctx)
{
	struct bpf_dynptr ptr1;
	struct bpf_dynptr ptr2;
	__u8 *p1, *p2;

	if (get_map_val_dynptr(&ptr1))
		return 0;
	if (get_map_val_dynptr(&ptr2))
		return 0;

	p1 = bpf_dynptr_data(&ptr1, 0, 1);
	if (!p1)
		return 0;
	p2 = bpf_dynptr_data(&ptr2, 0, 1);
	if (!p2)
		return 0;

	*(__u8 *)&ptr1 = 0;
	/* this should fail */
	return *p1;
}

/* Invalidation of slices should be scoped and should not prevent dereferencing
 * slices of another dynptr after destroying unrelated dynptr
 */
SEC("?raw_tp")
__success
int dynptr_invalidate_slice_success(void *ctx)
{
	struct bpf_dynptr ptr1;
	struct bpf_dynptr ptr2;
	__u8 *p1, *p2;

	if (get_map_val_dynptr(&ptr1))
		return 1;
	if (get_map_val_dynptr(&ptr2))
		return 1;

	p1 = bpf_dynptr_data(&ptr1, 0, 1);
	if (!p1)
		return 1;
	p2 = bpf_dynptr_data(&ptr2, 0, 1);
	if (!p2)
		return 1;

	*(__u8 *)&ptr1 = 0;
	return *p2;
}

/* Overwriting referenced dynptr should be rejected */
SEC("?raw_tp")
__failure __msg("cannot overwrite referenced dynptr")
int dynptr_overwrite_ref(void *ctx)
{
	struct bpf_dynptr ptr;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &ptr);
	/* this should fail */
	if (get_map_val_dynptr(&ptr))
		bpf_ringbuf_discard_dynptr(&ptr, 0);
	return 0;
}

/* Reject writes to dynptr slot from bpf_dynptr_read */
SEC("?raw_tp")
__failure __msg("potential write to dynptr at off=-16")
int dynptr_read_into_slot(void *ctx)
{
	union {
		struct {
			char _pad[48];
			struct bpf_dynptr ptr;
		};
		char buf[64];
	} data;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 64, 0, &data.ptr);
	/* this should fail */
	bpf_dynptr_read(data.buf, sizeof(data.buf), &data.ptr, 0, 0);

	return 0;
}

/* Reject writes to dynptr slot for uninit arg */
SEC("?raw_tp")
__failure __msg("potential write to dynptr at off=-16")
int uninit_write_into_slot(void *ctx)
{
	struct {
		char buf[64];
		struct bpf_dynptr ptr;
	} data;

	bpf_ringbuf_reserve_dynptr(&ringbuf, 80, 0, &data.ptr);
	/* this should fail */
	bpf_get_current_comm(data.buf, 80);

	return 0;
}

static int callback(__u32 index, void *data)
{
        *(__u32 *)data = 123;

        return 0;
}

/* If the dynptr is written into in a callback function, its data
 * slices should be invalidated as well.
 */
SEC("?raw_tp")
__failure __msg("invalid mem access 'scalar'")
int invalid_data_slices(void *ctx)
{
	struct bpf_dynptr ptr;
	__u32 *slice;

	if (get_map_val_dynptr(&ptr))
		return 0;

	slice = bpf_dynptr_data(&ptr, 0, sizeof(__u32));
	if (!slice)
		return 0;

	bpf_loop(10, callback, &ptr, 0);

	/* this should fail */
	*slice = 1;

	return 0;
}

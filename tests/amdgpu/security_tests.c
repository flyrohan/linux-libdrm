/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "CUnit/Basic.h"

#include "amdgpu_test.h"
#include "amdgpu_drm.h"
#include "amdgpu_internal.h"

#include <string.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/endian.h>
#else
#include <endian.h>
#endif
#include <strings.h>
#include <xf86drm.h>

static amdgpu_device_handle device_handle;
static uint32_t major_version;
static uint32_t minor_version;

static struct drm_amdgpu_info_hw_ip  sdma_info;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(_Arr)  (sizeof(_Arr)/sizeof((_Arr)[0]))
#endif


/* --------------------- Secure bounce test ------------------------ *
 *
 * The secure bounce test tests that we can evict a TMZ buffer,
 * and page it back in, via a bounce buffer, as it encryption/decryption
 * depends on its physical address, and have the same data, i.e. data
 * integrity is preserved.
 *
 * The steps are as follows (from Christian K.):
 *
 * Buffer A which is TMZ protected and filled by the CPU with a
 * certain pattern. That the GPU is reading only random nonsense from
 * that pattern is irrelevant for the test.
 *
 * This buffer A is then secure copied into buffer B which is also
 * TMZ protected.
 *
 * Buffer B is moved around, from VRAM to GTT, GTT to SYSTEM,
 * etc.
 *
 * Then, we use another secure copy of buffer B back to buffer A.
 *
 * And lastly we check with the CPU the pattern.
 *
 * Assuming that we don't have memory contention and buffer A stayed
 * at the same place, we should still see the same pattern when read
 * by the CPU.
 *
 * If we don't see the same pattern then something in the buffer
 * migration code is not working as expected.
 */

#define SECURE_BOUNCE_TEST_STR    "secure bounce"
#define SECURE_BOUNCE_FAILED_STR  SECURE_BOUNCE_TEST_STR " failed"

#define PRINT_ERROR(_Res)   fprintf(stderr, "%s:%d: %s (%d)\n",	\
				    __func__, __LINE__, strerror(-(_Res)), _Res)

#define PACKET_LCOPY_SIZE         7
#define PACKET_NOP_SIZE          12

#define PACKET_LWRITE_DATA_SIZE_IN_DWORDS	64
#define PACKET_LWRITE_DATA_SIZE	\
			(PACKET_LWRITE_DATA_SIZE_IN_DWORDS*4)

#define SECURE_BOUNCE_BUFFER_SIZE_IN_DWORDS  (4 * 1024)
#define SECURE_BOUNCE_BUFFER_SIZE \
			(SECURE_BOUNCE_BUFFER_SIZE_IN_DWORDS*4)

#define RAW_DATA 0xdeadbeef

struct sec_amdgpu_bo {
	struct amdgpu_bo *bo;
	struct amdgpu_va *va;
};

struct command_ctx {
	struct amdgpu_device    *dev;
	struct amdgpu_cs_ib_info cs_ibinfo;
	struct amdgpu_cs_request cs_req;
	struct amdgpu_context   *context;
	int ring_id;
};

/**
 * amdgpu_bo_alloc_map -- Allocate and map a buffer object (BO)
 * @dev: The AMDGPU device this BO belongs to.
 * @size: The size of the BO.
 * @alignment: Alignment of the BO.
 * @gem_domain: One of AMDGPU_GEM_DOMAIN_xyz.
 * @alloc_flags: One of AMDGPU_GEM_CREATE_xyz.
 * @sbo: the result
 *
 * Allocate a buffer object (BO) with the desired attributes
 * as specified by the argument list and write out the result
 * into @sbo.
 *
 * Return 0 on success and @sbo->bo and @sbo->va are set,
 * or -errno on error.
 */
static int amdgpu_bo_alloc_map(struct amdgpu_device *dev,
			       unsigned size,
			       unsigned alignment,
			       unsigned gem_domain,
			       uint64_t alloc_flags,
			       struct sec_amdgpu_bo *sbo)
{
	void *cpu;
	uint64_t mc_addr;

	return amdgpu_bo_alloc_and_map_raw(dev,
					   size,
					   alignment,
					   gem_domain,
					   alloc_flags,
					   0,
					   &sbo->bo,
					   &cpu, &mc_addr,
					   &sbo->va);
}

static void amdgpu_bo_unmap_free(struct sec_amdgpu_bo *sbo,
				 const uint64_t size)
{
	(void) amdgpu_bo_unmap_and_free(sbo->bo,
					sbo->va,
					sbo->va->address,
					size);
	sbo->bo = NULL;
	sbo->va = NULL;
}

static void amdgpu_sdma_lcopy(uint32_t *packet,
			      const uint64_t dst,
			      const uint64_t src,
			      const uint32_t size,
			      const int secure)
{
	/* Set the packet to Linear copy with TMZ set.
	 */
	packet[0] = htole32(secure << 18 | 1);
	packet[1] = htole32(size-1);
	packet[2] = htole32(0);
	packet[3] = htole32((uint32_t)(src & 0xFFFFFFFFU));
	packet[4] = htole32((uint32_t)(src >> 32));
	packet[5] = htole32((uint32_t)(dst & 0xFFFFFFFFU));
	packet[6] = htole32((uint32_t)(dst >> 32));
}

static void amdgpu_sdma_lwrite(uint32_t *packet,
                const uint64_t dst,
                const uint32_t *data,
                const uint32_t size,
                const int secure)
{
	int i;

	/* Set the packet to Linear write with TMZ set.
	*/
	packet[0] = htole32(secure << 18 | 2);
	packet[1] = htole32((uint32_t)(dst & 0xFFFFFFFFU));
	packet[2] = htole32((uint32_t)(dst >> 32));
	packet[3] = htole32(size-1);

	for (i = 0; i < size; i++)
		packet[4+i] = htole32(data[i]);
}

static void amdgpu_sdma_atomic(uint32_t *packet,
			const uint64_t dst,
			const uint32_t src_data,
			const uint32_t cmp_data,
			const int secure)
{
	/* Set the packet to Atomic and ATOMIC_SWAPCMP_RTN opcode with TMZ set
	 * loop, 1-loop_until_compare_satisfied
	 * single_pass_atomic, 0-lru
	 */
	packet[0] = htole32(1 << 28 | secure << 18 | 1 << 16 | 1 << 3 | 1 << 1);
	packet[1] = htole32((uint32_t)(dst & 0xFFFFFFFFU));
	packet[2] = htole32((uint32_t)(dst >> 32));
	packet[3] = htole32(src_data);
	packet[4] = htole32(0x0);
	packet[5] = htole32(cmp_data);
	packet[6] = htole32(0x0);
	packet[7] = htole32(0x100);
}

static void amdgpu_sdma_nop(uint32_t *packet, uint32_t nop_count)
{
	/* A packet of the desired number of NOPs.
	 */
	packet[0] = htole32(nop_count << 16);
	for ( ; nop_count > 0; nop_count--)
		packet[nop_count-1] = 0;
}

/**
 * amdgpu_bo_lcopy -- linear copy with TMZ set, using sDMA
 * @dev: AMDGPU device to which both buffer objects belong to
 * @dst: destination buffer object
 * @src: source buffer object
 * @size: size of memory to move, in bytes.
 * @secure: Set to 1 to perform secure copy, 0 for clear
 *
 * Issues and waits for completion of a Linear Copy with TMZ
 * set, to the sDMA engine. @size should be a multiple of
 * at least 16 bytes.
 */
static void amdgpu_bo_lcopy(struct command_ctx *ctx,
			    struct sec_amdgpu_bo *dst,
			    struct sec_amdgpu_bo *src,
			    const uint32_t size,
			    int secure)
{
	struct amdgpu_bo *bos[] = { dst->bo, src->bo };
	uint32_t packet[PACKET_LCOPY_SIZE];

	amdgpu_sdma_lcopy(packet,
			  dst->va->address,
			  src->va->address,
			  size, secure);
	amdgpu_test_exec_cs_helper_raw(ctx->dev, ctx->context,
				       AMDGPU_HW_IP_DMA, ctx->ring_id,
				       ARRAY_SIZE(packet), packet,
				       ARRAY_SIZE(bos), bos,
				       &ctx->cs_ibinfo, &ctx->cs_req,
				       secure == 1);
}

/**
 * amdgpu_bo_lwrite -- linear write with TMZ set, using sDMA
 * @ctx: AMDGPU ctx to which both buffer objects belong to
 * @dst: destination buffer object
 * @data: source buffer object
 * @size: size of source buffer object, in bytes.
 * @secure: Set to 1 to perform secure write, 0 for clear
 *
 * Issues and waits for completion of a Linear Write with TMZ
 * set, to the sDMA engine. @size should be a multiple of
 * at least 16 bytes.
 */
static void amdgpu_bo_lwrite(struct command_ctx *ctx,
                struct sec_amdgpu_bo *dst,
                uint32_t *data,
                const uint32_t size,
                int secure)
{
	struct amdgpu_bo *bos[] = { dst->bo };
	uint32_t packet[PACKET_LWRITE_DATA_SIZE/sizeof(uint32_t)+4];

	amdgpu_sdma_lwrite(packet,
			  dst->va->address, data,
			  size, secure);

	amdgpu_test_exec_cs_helper_raw(ctx->dev, ctx->context,
				       AMDGPU_HW_IP_DMA, ctx->ring_id,
				       ARRAY_SIZE(packet), packet,
				       ARRAY_SIZE(bos), bos,
				       &ctx->cs_ibinfo, &ctx->cs_req,
				       secure == 1);
}

static void amdgpu_bo_compare(struct command_ctx *ctx,
                struct sec_amdgpu_bo *dst,
                int offset_in_bytes,
                uint32_t src_data,
                uint32_t cmp_ata,
                int secure)
{
	struct amdgpu_bo *bos[] = { dst->bo };
	uint32_t packet[8];

	amdgpu_sdma_atomic(packet,
			  dst->va->address+offset_in_bytes,
			  src_data, cmp_ata, secure);

	amdgpu_test_exec_cs_helper_raw(ctx->dev, ctx->context,
				       AMDGPU_HW_IP_DMA, ctx->ring_id,
				       ARRAY_SIZE(packet), packet,
				       ARRAY_SIZE(bos), bos,
				       &ctx->cs_ibinfo, &ctx->cs_req,
				       secure == 1);
}

/**
 * amdgpu_bo_move -- Evoke a move of the buffer object (BO)
 * @dev: device to which this buffer object belongs to
 * @bo: the buffer object to be moved
 * @whereto: one of AMDGPU_GEM_DOMAIN_xyz
 * @secure: set to 1 to submit secure IBs
 *
 * Evokes a move of the buffer object @bo to the GEM domain
 * descibed by @whereto.
 *
 * Returns 0 on sucess; -errno on error.
 */
static int amdgpu_bo_move(struct command_ctx *ctx,
			  struct amdgpu_bo *bo,
			  uint64_t whereto,
			  int secure)
{
	struct amdgpu_bo *bos[] = { bo };
	struct drm_amdgpu_gem_op gop = {
		.handle  = bo->handle,
		.op      = AMDGPU_GEM_OP_SET_PLACEMENT,
		.value   = whereto,
	};
	uint32_t packet[PACKET_NOP_SIZE];
	int res;

	/* Change the buffer's placement.
	 */
	res = drmIoctl(ctx->dev->fd, DRM_IOCTL_AMDGPU_GEM_OP, &gop);
	if (res)
		return -errno;

	/* Now issue a NOP to actually evoke the MM to move
	 * it to the desired location.
	 */
	amdgpu_sdma_nop(packet, PACKET_NOP_SIZE);
	amdgpu_test_exec_cs_helper_raw(ctx->dev, ctx->context,
				       AMDGPU_HW_IP_DMA, ctx->ring_id,
				       ARRAY_SIZE(packet), packet,
				       ARRAY_SIZE(bos), bos,
				       &ctx->cs_ibinfo, &ctx->cs_req,
				       secure == 1);
	return 0;
}

/**
 * amdgpu_secure_sdma_lcopy
 * sdma linear copy from Alice buffer to Bob buffer
 * @alice_encrypted: the Alice buffer is encrypted or not
 * @bob_encrypted: the Bob buffer is encrypted or not
 * @tmz_mode: tmz mode or not
 */
static void amdgpu_secure_sdma_lcopy(bool alice_encrypted,
			  bool bob_encrypted, int tmz_mode)
{
	struct sec_amdgpu_bo alice, bob;
	struct command_ctx   sb_ctx;
	long page_size;
	uint32_t data[PACKET_LWRITE_DATA_SIZE_IN_DWORDS];
	int i, res;

	page_size = sysconf(_SC_PAGESIZE);

	memset(&sb_ctx, 0, sizeof(sb_ctx));
	sb_ctx.dev = device_handle;
	res = amdgpu_cs_ctx_create(sb_ctx.dev, &sb_ctx.context);
	if (res) {
		PRINT_ERROR(res);
		return;
	}

	/* Use the first present ring.
	 */
	res = ffs(sdma_info.available_rings) - 1;
	if (res == -1) {
		PRINT_ERROR(-ENOENT);
		goto Out_free_ctx;
	}
	sb_ctx.ring_id = res;

	/* Allocate a buffer named Alice in VRAM.
	 */
	res = amdgpu_bo_alloc_map(device_handle,
				       PACKET_LWRITE_DATA_SIZE,
				       page_size,
				       AMDGPU_GEM_DOMAIN_VRAM,
				       alice_encrypted ? AMDGPU_GEM_CREATE_ENCRYPTED : 0,
				       &alice);

	if (res) {
		PRINT_ERROR(res);
		return;
	}

	for (i = 0; i < PACKET_LWRITE_DATA_SIZE_IN_DWORDS; i++)
		data[i] = RAW_DATA;

	/* Fill Alice with a pattern.
	 */
	amdgpu_bo_lwrite(&sb_ctx, &alice, data, PACKET_LWRITE_DATA_SIZE_IN_DWORDS, tmz_mode);

	/* Allocate a buffer named Bob in VRAM.
	 */
	res = amdgpu_bo_alloc_map(device_handle,
				       PACKET_LWRITE_DATA_SIZE,
				       page_size,
				       AMDGPU_GEM_DOMAIN_VRAM,
				       bob_encrypted ? AMDGPU_GEM_CREATE_ENCRYPTED : 0,
				       &bob);

	if (res) {
		PRINT_ERROR(res);
		goto Out_free_Alice;
	}

	/* sDMA copy from Alice to Bob.
	 */
	amdgpu_bo_lcopy(&sb_ctx, &bob, &alice, PACKET_LWRITE_DATA_SIZE, tmz_mode);

	/* For linear write to Alice buffer,
	 * Only when Alice's buffer is a regular buffer, then in non-TMZ mode
	 * the data will not be encrypted.
	 */
	res = memcmp(alice.bo->cpu_ptr, (void*)&data[0], PACKET_LWRITE_DATA_SIZE);
	if (!alice_encrypted && !tmz_mode) {
		CU_ASSERT_EQUAL(res, 0);
	} else {
		CU_ASSERT_NOT_EQUAL(res, 0);
	}

	/* For linear copy from Alice to Bob buffer,
	 * Only when Bob's buffer is a regular buffer, then in non-TMZ mode
	 * the data will not be encrypted.
	 */
	res = memcmp(alice.bo->cpu_ptr, bob.bo->cpu_ptr, PACKET_LWRITE_DATA_SIZE);
	if (!bob_encrypted && !tmz_mode) {
		CU_ASSERT_EQUAL(res, 0);
	} else {
		CU_ASSERT_NOT_EQUAL(res, 0);
	}

	/* When performing a linear write from Bob to Alice in TMZ mode and
	 * Alice/Bob buffer are encrypted buffer, we expect that the data
	 * will be equal to the original raw data.
	 */
	if (!!tmz_mode && alice_encrypted && bob_encrypted) {
		uint32_t bo_cpu_origin;
		for (i = 0; i < PACKET_LWRITE_DATA_SIZE_IN_DWORDS; i++) {
			bo_cpu_origin = *((uint32_t*)bob.bo->cpu_ptr+i);
			amdgpu_bo_compare(&sb_ctx, &bob, i*4, 0x12345678, data[i], tmz_mode);
			CU_ASSERT_NOT_EQUAL(*((uint32_t*)bob.bo->cpu_ptr+i), bo_cpu_origin);
		}
	}

Out_free_Alice:
	amdgpu_bo_unmap_free(&alice, PACKET_LWRITE_DATA_SIZE);
Out_free_ctx:
	res = amdgpu_cs_ctx_free(sb_ctx.context);
	CU_ASSERT_EQUAL(res, 0);
}

static void amdgpu_secure_sdma_lcopy_tests()
{
	amdgpu_secure_sdma_lcopy(false, false, 0);
	amdgpu_secure_sdma_lcopy(true, false, 0);
	amdgpu_secure_sdma_lcopy(false, true, 0);
	amdgpu_secure_sdma_lcopy(true, true, 0);
	amdgpu_secure_sdma_lcopy(false, false, 1);
	amdgpu_secure_sdma_lcopy(true, false, 1);
	amdgpu_secure_sdma_lcopy(false, true, 1);
	amdgpu_secure_sdma_lcopy(true, true, 1);
}

static void amdgpu_secure_bounce(void)
{
	struct sec_amdgpu_bo alice, bob, charlie, dave;
	struct command_ctx sb_ctx;
	long page_size;
	int res;
	int i;
	uint32_t data[SECURE_BOUNCE_BUFFER_SIZE_IN_DWORDS];
	uint32_t bo_cpu_origin;

	page_size = sysconf(_SC_PAGESIZE);

	memset(&sb_ctx, 0, sizeof(sb_ctx));
	sb_ctx.dev = device_handle;
	res = amdgpu_cs_ctx_create(sb_ctx.dev, &sb_ctx.context);
	if (res) {
		PRINT_ERROR(res);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		return;
	}

	/* Use the first present ring.
	 */
	res = ffs(sdma_info.available_rings) - 1;
	if (res == -1) {
		PRINT_ERROR(-ENOENT);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		goto Out_free_ctx;
	}
	sb_ctx.ring_id = res;

	/* Allocate a buffer named alice/bob/charlie/dave in VRAM.
	 * alice and dave are non-encrypted buffers.
	 * bob and charlie are encrypted buffers.
	 */
	res = amdgpu_bo_alloc_map(device_handle,
			       SECURE_BOUNCE_BUFFER_SIZE,
			       page_size,
			       AMDGPU_GEM_DOMAIN_VRAM,
			       0,
			       &alice);
	if (res) {
		PRINT_ERROR(res);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		return;
	}

	res = amdgpu_bo_alloc_map(device_handle,
			       SECURE_BOUNCE_BUFFER_SIZE,
			       page_size,
			       AMDGPU_GEM_DOMAIN_VRAM,
			       AMDGPU_GEM_CREATE_ENCRYPTED,
			       &bob);
	if (res) {
		PRINT_ERROR(res);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		goto Out_free_A;
	}

	res = amdgpu_bo_alloc_map(device_handle,
			       SECURE_BOUNCE_BUFFER_SIZE,
			       page_size,
			       AMDGPU_GEM_DOMAIN_VRAM,
			       AMDGPU_GEM_CREATE_ENCRYPTED,
			       &charlie);
	if (res) {
		PRINT_ERROR(res);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		goto Out_free_B;
	}

	res = amdgpu_bo_alloc_map(device_handle,
			       SECURE_BOUNCE_BUFFER_SIZE,
			       page_size,
			       AMDGPU_GEM_DOMAIN_VRAM,
			       0,
			       &dave);
	if (res) {
		PRINT_ERROR(res);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		goto Out_free_C;
	}

	for (i = 0; i < SECURE_BOUNCE_BUFFER_SIZE_IN_DWORDS; i++)
		data[i] = RAW_DATA;

	/* Fill alice with a pattern.
	 */
	memcpy(alice.bo->cpu_ptr, data, SECURE_BOUNCE_BUFFER_SIZE);

	/* sDMA secure copy from alice to bob.
	 */
	amdgpu_bo_lcopy(&sb_ctx, &bob, &alice, SECURE_BOUNCE_BUFFER_SIZE, 1);

	/* Move bob to the GTT domain.
	 */
	res = amdgpu_bo_move(&sb_ctx, bob.bo, AMDGPU_GEM_DOMAIN_GTT, 0);
	if (res) {
		PRINT_ERROR(res);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		goto Out_free_all;
	}

	/* sDMA secure copy from bob to charlie.
	 */
	amdgpu_bo_lcopy(&sb_ctx, &charlie, &bob, SECURE_BOUNCE_BUFFER_SIZE, 1);

	/* Move charlie to the GTT domain.
	 */
	res = amdgpu_bo_move(&sb_ctx, charlie.bo, AMDGPU_GEM_DOMAIN_GTT, 0);
	if (res) {
		PRINT_ERROR(res);
		CU_FAIL(SECURE_BOUNCE_FAILED_STR);
		goto Out_free_all;
	}

	/* sDMA clear copy from charlie to dave.
	 */
	amdgpu_bo_lcopy(&sb_ctx, &dave, &charlie, SECURE_BOUNCE_BUFFER_SIZE, 0);

	/* Verify the contents of alice.
	 */
	CU_ASSERT_EQUAL(*((uint32_t*)alice.bo->cpu_ptr), RAW_DATA);

	/* Verify the contents of bob.
	 * The encrypted data of bob buffer should be same raw data.
	 */
	for (i = 0; i < SECURE_BOUNCE_BUFFER_SIZE_IN_DWORDS; i++) {
		bo_cpu_origin = *((uint32_t*)bob.bo->cpu_ptr+i);
		amdgpu_bo_compare(&sb_ctx, &bob, i*4, 0x12345678, RAW_DATA, 1);
		CU_ASSERT_NOT_EQUAL(*((uint32_t*)bob.bo->cpu_ptr+i), bo_cpu_origin);
	}

	/* Verify the contents of charlie.
	 * The encrypted data of charlie buffer should be same raw data.
	 */
	for (i = 0; i < SECURE_BOUNCE_BUFFER_SIZE_IN_DWORDS; i++) {
		bo_cpu_origin = *((uint32_t*)charlie.bo->cpu_ptr+i);
		amdgpu_bo_compare(&sb_ctx, &charlie, i*4, 0x12345678, RAW_DATA, 1);
		CU_ASSERT_NOT_EQUAL(*((uint32_t*)charlie.bo->cpu_ptr+i), bo_cpu_origin);
	}

	/* Verify the contents of dave.
	 * The encrypted data of dave buffer should be different to raw data.
	 */
	for (i = 0; i < SECURE_BOUNCE_BUFFER_SIZE_IN_DWORDS; i++) {
		bo_cpu_origin = *((uint32_t*)dave.bo->cpu_ptr+i);
		amdgpu_bo_compare(&sb_ctx, &dave, i*4, 0x12345678, RAW_DATA, 1);
		CU_ASSERT_EQUAL(*((uint32_t*)dave.bo->cpu_ptr+i), bo_cpu_origin);
	}

Out_free_all:
	amdgpu_bo_unmap_free(&dave, SECURE_BOUNCE_BUFFER_SIZE);
Out_free_C:
	amdgpu_bo_unmap_free(&charlie, SECURE_BOUNCE_BUFFER_SIZE);
Out_free_B:
	amdgpu_bo_unmap_free(&bob, SECURE_BOUNCE_BUFFER_SIZE);
Out_free_A:
	amdgpu_bo_unmap_free(&alice, SECURE_BOUNCE_BUFFER_SIZE);
Out_free_ctx:
	res = amdgpu_cs_ctx_free(sb_ctx.context);
	CU_ASSERT_EQUAL(res, 0);
}


/* ----------------------------------------------------------------- */

static void amdgpu_security_alloc_buf_test(void)
{
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint64_t bo_mc;
	int r;

	/* Test secure buffer allocation in VRAM */
	bo = gpu_mem_alloc(device_handle, 4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_ENCRYPTED,
			   &bo_mc, &va_handle);

	r = gpu_mem_free(bo, va_handle, bo_mc, 4096);
	CU_ASSERT_EQUAL(r, 0);

	/* Test secure buffer allocation in system memory */
	bo = gpu_mem_alloc(device_handle, 4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   AMDGPU_GEM_CREATE_ENCRYPTED,
			   &bo_mc, &va_handle);

	r = gpu_mem_free(bo, va_handle, bo_mc, 4096);
	CU_ASSERT_EQUAL(r, 0);

	/* Test secure buffer allocation in invisible VRAM */
	bo = gpu_mem_alloc(device_handle, 4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   AMDGPU_GEM_CREATE_ENCRYPTED |
			   AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
			   &bo_mc, &va_handle);

	r = gpu_mem_free(bo, va_handle, bo_mc, 4096);
	CU_ASSERT_EQUAL(r, 0);
}

static void amdgpu_security_gfx_submission_test(void)
{
	amdgpu_command_submission_write_linear_helper_with_secure(device_handle,
								  AMDGPU_HW_IP_GFX,
								  true);
}

static void amdgpu_security_sdma_submission_test(void)
{
	amdgpu_command_submission_write_linear_helper_with_secure(device_handle,
								  AMDGPU_HW_IP_DMA,
								  true);
}

/* ----------------------------------------------------------------- */

CU_TestInfo security_tests[] = {
	{ "allocate secure buffer test",        amdgpu_security_alloc_buf_test },
	{ "graphics secure command submission", amdgpu_security_gfx_submission_test },
	{ "sDMA secure command submission",     amdgpu_security_sdma_submission_test },
	{ "sDMA secure linear copy test",       amdgpu_secure_sdma_lcopy_tests },
	{ SECURE_BOUNCE_TEST_STR,               amdgpu_secure_bounce },
	CU_TEST_INFO_NULL,
};

CU_BOOL suite_security_tests_enable(void)
{
	CU_BOOL enable = CU_TRUE;

	if (amdgpu_device_initialize(drm_amdgpu[0], &major_version,
				     &minor_version, &device_handle))
		return CU_FALSE;


	if (!(device_handle->dev_info.ids_flags & AMDGPU_IDS_FLAGS_TMZ)) {
		printf("\n\nDon't support TMZ (trust memory zone), security suite disabled\n");
		enable = CU_FALSE;
	}

	if ((major_version < 3) ||
		((major_version == 3) && (minor_version < 37))) {
		printf("\n\nDon't support TMZ (trust memory zone), kernel DRM version (%d.%d)\n",
			major_version, minor_version);
		printf("is older, security suite disabled\n");
		enable = CU_FALSE;
	}

	if (amdgpu_device_deinitialize(device_handle))
		return CU_FALSE;

	return enable;
}

int suite_security_tests_init(void)
{
	int res;

	res = amdgpu_device_initialize(drm_amdgpu[0], &major_version,
				       &minor_version, &device_handle);
	if (res) {
		PRINT_ERROR(res);
		return CUE_SINIT_FAILED;
	}

	res = amdgpu_query_hw_ip_info(device_handle,
				      AMDGPU_HW_IP_DMA,
				      0, &sdma_info);
	if (res) {
		PRINT_ERROR(res);
		return CUE_SINIT_FAILED;
	}

	return CUE_SUCCESS;
}

int suite_security_tests_clean(void)
{
	int res;

	res = amdgpu_device_deinitialize(device_handle);
	if (res)
		return CUE_SCLEAN_FAILED;

	return CUE_SUCCESS;
}

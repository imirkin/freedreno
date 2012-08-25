/*
 * Copyright (c) 2012 Rob Clark <robdclark@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ir.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "util.h"
#include "instr.h"

#define REG_MASK 0x3f	/* not really sure how many regs yet */
#define ADDR_MASK 0xfff

static int cf_emit(struct ir_cf *cf1, struct ir_cf *cf2, uint32_t *dwords);

static int instr_emit(struct ir_instruction *instr, uint32_t *dwords,
		uint32_t idx, struct ir_shader_info *info);

static void reg_update_stats(struct ir_register *reg,
		struct ir_shader_info *info, bool dest);
static uint32_t reg_fetch_src_swiz(struct ir_register *reg, uint32_t n);
static uint32_t reg_fetch_dst_swiz(struct ir_register *reg);
static uint32_t reg_alu_dst_swiz(struct ir_register *reg);
static uint32_t reg_alu_src_swiz(struct ir_register *reg);

/* simple allocator to carve allocations out of an up-front allocated heap,
 * so that we can free everything easily in one shot.
 */
static void * ir_alloc(struct ir_shader *shader, int sz)
{
	void *ptr = &shader->heap[shader->heap_idx];
	shader->heap_idx += ALIGN(sz, 4);
	return ptr;
}

static char * ir_strdup(struct ir_shader *shader, const char *str)
{
	char *ptr = NULL;
	if (str) {
		int len = strlen(str);
		ptr = ir_alloc(shader, len+1);
		memcpy(ptr, str, len);
		ptr[len] = '\0';
	}
	return ptr;
}

struct ir_shader * ir_shader_create(void)
{
	DEBUG_MSG("");
	return calloc(1, sizeof(struct ir_shader));
}

void ir_shader_destroy(struct ir_shader *shader)
{
	DEBUG_MSG("");
	free(shader);
}

/* resolve addr/cnt/sequence fields in the individual CF's */
static int shader_resolve(struct ir_shader *shader)
{
	uint32_t addr;
	unsigned i;
	int j;

	addr = shader->cfs_count / 2;
	for (i = 0; i < shader->cfs_count; i++) {
		struct ir_cf *cf = shader->cfs[i];
		if ((cf->cf_type == T_EXEC) || (cf->cf_type == T_EXEC_END)) {
			uint32_t sequence = 0;

			if (cf->exec.addr && (cf->exec.addr != addr))
				WARN_MSG("invalid addr '%d' at CF %d", cf->exec.addr, i);
			if (cf->exec.cnt && (cf->exec.cnt != cf->exec.instrs_count))
				WARN_MSG("invalid cnt '%d' at CF %d", cf->exec.cnt, i);

			for (j = cf->exec.instrs_count - 1; j >= 0; j--) {
				struct ir_instruction *instr = cf->exec.instrs[j];
				sequence <<= 2;
				if (instr->instr_type == T_FETCH)
					sequence |= 0x1;
				if (instr->sync)
					sequence |= 0x2;
			}

			cf->exec.addr = addr;
			cf->exec.cnt  = cf->exec.instrs_count;
			cf->exec.sequence = sequence;

			addr += cf->exec.instrs_count;
		}
	}

	return 0;
}

int ir_shader_assemble(struct ir_shader *shader,
		uint32_t *dwords, int sizedwords,
		struct ir_shader_info *info)
{
	uint32_t i, j;
	uint32_t *ptr = dwords;
	uint32_t idx = 0;
	int ret;

	info->max_reg       = -1;
	info->max_input_reg = 0;
	info->regs_written  = 0;

	/* we need an even # of CF's.. insert a NOP if needed */
	if (shader->cfs_count != ALIGN(shader->cfs_count, 2))
		ir_cf_create(shader, T_NOP);

	/* first pass, resolve sizes and addresses: */
	ret = shader_resolve(shader);
	if (ret) {
		ERROR_MSG("resolve failed: %d", ret);
		return ret;
	}

	/* second pass, emit CF program in pairs: */
	for (i = 0; i < shader->cfs_count; i += 2) {
		ret = cf_emit(shader->cfs[i], shader->cfs[i+1], ptr);
		if (ret) {
			ERROR_MSG("CF emit failed: %d\n", ret);
			return ret;
		}
		ptr += 3;
	}

	/* third pass, emit ALU/FETCH: */
	for (i = 0; i < shader->cfs_count; i++) {
		struct ir_cf *cf = shader->cfs[i];
		if ((cf->cf_type == T_EXEC) || (cf->cf_type == T_EXEC_END)) {
			for (j = 0; j < cf->exec.instrs_count; j++) {
				ret = instr_emit(cf->exec.instrs[j], ptr, idx++, info);
				if (ret) {
					ERROR_MSG("instruction emit failed: %d", ret);
					return ret;
				}
				ptr += 3;
			}
		}
	}

	return ptr - dwords;
}


struct ir_attribute * ir_attribute_create(struct ir_shader *shader,
		int rstart, int num, const char *name)
{
	struct ir_attribute *a = ir_alloc(shader, sizeof(struct ir_attribute));
	DEBUG_MSG("R%d-R%d: %s", rstart, rstart + num - 1, name);
	a->name   = ir_strdup(shader, name);
	a->rstart = rstart;
	a->num    = num;
	assert(shader->attributes_count < ARRAY_SIZE(shader->attributes));
	shader->attributes[shader->attributes_count++] = a;
	return a;
}

struct ir_const * ir_const_create(struct ir_shader *shader,
		int cstart, float v0, float v1, float v2, float v3)
{
	struct ir_const *c = ir_alloc(shader, sizeof(struct ir_const));
	DEBUG_MSG("C%d: %f, %f, %f, %f", cstart, v0, v1, v2, v3);
	c->val[0] = v0;
	c->val[1] = v1;
	c->val[2] = v2;
	c->val[3] = v3;
	c->cstart = cstart;
	assert(shader->consts_count < ARRAY_SIZE(shader->consts));
	shader->consts[shader->consts_count++] = c;
	return c;
}

struct ir_sampler * ir_sampler_create(struct ir_shader *shader,
		int idx, const char *name)
{
	struct ir_sampler *s = ir_alloc(shader, sizeof(struct ir_sampler));
	DEBUG_MSG("CONST(%d): %s", idx, name);
	s->name   = ir_strdup(shader, name);
	s->idx    = idx;
	assert(shader->samplers_count < ARRAY_SIZE(shader->samplers));
	shader->samplers[shader->samplers_count++] = s;
	return s;
}

struct ir_uniform * ir_uniform_create(struct ir_shader *shader,
		int cstart, int num, const char *name)
{
	struct ir_uniform *u = ir_alloc(shader, sizeof(struct ir_uniform));
	DEBUG_MSG("C%d-C%d: %s", cstart, cstart + num - 1, name);
	u->name   = ir_strdup(shader, name);
	u->cstart = cstart;
	u->num    = num;
	assert(shader->uniforms_count < ARRAY_SIZE(shader->uniforms));
	shader->uniforms[shader->uniforms_count++] = u;
	return u;
}

struct ir_varying * ir_varying_create(struct ir_shader *shader,
		int rstart, int num, const char *name)
{
	struct ir_varying *v = ir_alloc(shader, sizeof(struct ir_varying));
	DEBUG_MSG("R%d-R%d: %s", rstart, rstart + num - 1, name);
	v->name   = ir_strdup(shader, name);
	v->rstart = rstart;
	v->num    = num;
	assert(shader->varyings_count < ARRAY_SIZE(shader->varyings));
	shader->varyings[shader->varyings_count++] = v;
	return v;
}


struct ir_cf * ir_cf_create(struct ir_shader *shader, int cf_type)
{
	struct ir_cf *cf = ir_alloc(shader, sizeof(struct ir_cf));
	DEBUG_MSG("%d", cf_type);
	cf->shader = shader;
	cf->cf_type = cf_type;
	assert(shader->cfs_count < ARRAY_SIZE(shader->cfs));
	shader->cfs[shader->cfs_count++] = cf;
	return cf;
}

static uint32_t cf_op(struct ir_cf *cf)
{
	switch (cf->cf_type) {
	default:
		ERROR_MSG("invalid CF: %d\n", cf->cf_type);
	case T_NOP:         return 0x0;
	case T_EXEC:        return 0x1;
	case T_EXEC_END:    return 0x2;
	case T_ALLOC:       return 0xc;
	}
}

/*
 * CF instruction format:
 * -- ----------- ------
 *
 *     dword0:   0..11   -  addr/size 1
 *              12..15   -  count 1
 *              16..31   -  sequence 1.. 2 bits per instruction in the EXEC
 *                          clause, the low bit seems to control FETCH vs
 *                          ALU instruction type, the high bit seems to be
 *                          (S) modifier on instruction (which might make
 *                          the name SERIALIZE() in optimize-for-adreno.pdf
 *                          make sense.. although I don't quite understand
 *                          the meaning yet)
 *
 *     dword1:   0..7    -  <UNKNOWN>
 *               8..15?  -  op 1
 *              16..27   -  addr/size 2
 *              28..31   -  count 2
 *
 *     dword2:   0..15   -  sequence 2
 *              16..23   -  <UNKNOWN>
 *              24..31   -  op 2
 */

static int cf_emit(struct ir_cf *cf1, struct ir_cf *cf2, uint32_t *dwords)
{
	dwords[0] = dwords[1] = dwords[2] = 0;

	dwords[1] |= cf_op(cf1) << 12;
	dwords[2] |= cf_op(cf2) << 28;

	switch (cf1->cf_type) {
	case T_EXEC:
	case T_EXEC_END:
		assert(cf1->exec.addr <= ADDR_MASK);
		assert(cf1->exec.cnt <= 0xf);
		assert(cf1->exec.sequence <= 0xffff);
		dwords[0] |= cf1->exec.addr;
		dwords[0] |= cf1->exec.cnt << 12;
		dwords[0] |= cf1->exec.sequence << 16;
		break;
	case T_ALLOC:
		assert(cf1->alloc.size <= ADDR_MASK);
		dwords[0] |= cf1->alloc.size;
		dwords[1] |= ((cf1->alloc.type == T_COORD) ? 0x2 : 0x4) << 8;
		break;
	}

	switch (cf2->cf_type) {
	case T_EXEC:
	case T_EXEC_END:
		assert(cf2->exec.addr <= ADDR_MASK);
		assert(cf2->exec.cnt <= 0xf);
		assert(cf2->exec.sequence <= 0xffff);
		dwords[1] |= cf2->exec.addr << 16;
		dwords[1] |= cf2->exec.cnt << 28;
		dwords[2] |= cf2->exec.sequence << 0;
		break;
	case T_ALLOC:
		assert(cf2->alloc.size <= ADDR_MASK);
		dwords[1] |= cf2->alloc.size << 16;
		dwords[2] |= ((cf2->alloc.type == T_COORD) ? 0x2 : 0x4) << 24;
		break;
	}

	return 0;
}


struct ir_instruction * ir_instr_create(struct ir_cf *cf, int instr_type)
{
	struct ir_instruction *instr =
			ir_alloc(cf->shader, sizeof(struct ir_instruction));
	DEBUG_MSG("%d", instr_type);
	instr->shader = cf->shader;
	instr->instr_type = instr_type;
	assert(cf->exec.instrs_count < ARRAY_SIZE(cf->exec.instrs));
	cf->exec.instrs[cf->exec.instrs_count++] = instr;
	return instr;
}

static uint32_t instr_fetch_opc(struct ir_instruction *instr)
{
	switch (instr->fetch.opc) {
	default:
		ERROR_MSG("invalid fetch opc: %d\n", instr->fetch.opc);
	case T_SAMPLE: return 0x01;
	case T_VERTEX: return 0x00;
	}
}

static uint32_t instr_vector_opc(struct ir_instruction *instr)
{
	switch (instr->alu.vector_opc) {
	default:
		ERROR_MSG("invalid vector opc: %d\n", instr->alu.vector_opc);
#define OPC(x) case T_##x: return x
	OPC(ADDv);
	OPC(MULv);
	OPC(MAXv);
	OPC(MINv);
	OPC(SETEv);
	OPC(SETGTv);
	OPC(SETGTEv);
	OPC(SETNEv);
	OPC(FRACv);
	OPC(TRUNCv);
	OPC(FLOORv);
	OPC(MULADDv);
	OPC(CNDEv);
	OPC(CNDGTEv);
	OPC(CNDGTv);
	OPC(DOT4v);
	OPC(DOT3v);
	OPC(DOT2ADDv);
	OPC(CUBEv);
	OPC(MAX4v);
	OPC(PRED_SETE_PUSHv);
	OPC(PRED_SETNE_PUSHv);
	OPC(PRED_SETGT_PUSHv);
	OPC(PRED_SETGTE_PUSHv);
	OPC(KILLEv);
	OPC(KILLGTv);
	OPC(KILLGTEv);
	OPC(KILLNEv);
	OPC(DSTv);
	OPC(MOVAv);
	}
}

static uint32_t instr_scalar_opc(struct ir_instruction *instr)
{
	switch (instr->alu.scalar_opc) {
	default:
		ERROR_MSG("invalid scalar: %d\n", instr->alu.scalar_opc);
	OPC(ADDs);
	OPC(ADD_PREVs);
	OPC(MULs);
	OPC(MUL_PREVs);
	OPC(MUL_PREV2s);
	OPC(MAXs);
	OPC(MINs);
	OPC(SETEs);
	OPC(SETGTs);
	OPC(SETGTEs);
	OPC(SETNEs);
	OPC(FRACs);
	OPC(TRUNCs);
	OPC(FLOORs);
	OPC(EXP_IEEE);
	OPC(LOG_CLAMP);
	OPC(LOG_IEEE);
	OPC(RECIP_CLAMP);
	OPC(RECIP_FF);
	OPC(RECIP_IEEE);
	OPC(RECIPSQ_CLAMP);
	OPC(RECIPSQ_FF);
	OPC(RECIPSQ_IEEE);
	OPC(MOVAs);
	OPC(MOVA_FLOORs);
	OPC(SUBs);
	OPC(SUB_PREVs);
	OPC(PRED_SETEs);
	OPC(PRED_SETNEs);
	OPC(PRED_SETGTs);
	OPC(PRED_SETGTEs);
	OPC(PRED_SET_INVs);
	OPC(PRED_SET_POPs);
	OPC(PRED_SET_CLRs);
	OPC(PRED_SET_RESTOREs);
	OPC(KILLEs);
	OPC(KILLGTs);
	OPC(KILLGTEs);
	OPC(KILLNEs);
	OPC(KILLONEs);
	OPC(SQRT_IEEE);
	OPC(MUL_CONST_0);
	OPC(MUL_CONST_1);
	OPC(ADD_CONST_0);
	OPC(ADD_CONST_1);
	OPC(SUB_CONST_0);
	OPC(SUB_CONST_1);
	OPC(SIN);
	OPC(COS);
	OPC(RETAIN_PREV);
#undef OPC
	}
}

/*
 * VTX FETCH instruction format:
 * --- ----- ----------- ------
 *
 *     dword0:   0..4?   -  fetch operation - 0x00
 *               5..10?  -  src register
 *                11     -  <UNKNOWN>
 *              12..17?  -  dest register
 *             18?..19   -  <UNKNOWN>
 *              20..23?  -  const
 *              24..25   -  <UNKNOWN>  (maybe part of const?)
 *              25..26   -  src swizzle (x)
 *                            00 - x
 *                            01 - y
 *                            10 - z
 *                            11 - w
 *              27..31   -  unknown
 *
 *     dword1:   0..11   -  dest swizzle/mask, 3 bits per channel (w/z/y/x),
 *                          low two bits of each determine position src channel,
 *                          high bit set 1 to mask
 *                12     -  signedness ('1' signed, '0' unsigned)
 *              13..15   -  <UNKNOWN>
 *              16..21?  -  type - see 'enum SURFACEFORMAT'
 *             22?..31   -  <UNKNOWN>
 *
 *     dword2:   0..15   -  stride (more than 0xff and data is copied/packed)
 *              16..31   -  <UNKNOWN>
 *
 * note: at least VERTEX fetch instructions get patched up at runtime
 * based on the size of attributes attached.. for example, if vec4, but
 * size given in glVertexAttribPointer() then the write mask and size
 * (stride?) is updated
 *
 * TEX FETCH instruction format:
 * --- ----- ----------- ------
 *
 *     dword0:   0..4?   -  fetch operation - 0x01
 *               5..10?  -  src register
 *                11     -  <UNKNOWN>
 *              12..17?  -  dest register
 *             18?..19   -  <UNKNOWN>
 *              20..23?  -  const
 *              24..25   -  <UNKNOWN>  (maybe part of const?)
 *              26..31   -  src swizzle (z/y/x)
 *                            00 - x
 *                            01 - y
 *                            10 - z
 *                            11 - w
 *
 *     dword1:   0..11   -  dest swizzle/mask, 3 bits per channel (w/z/y/x),
 *                          low two bits of each determine position src channel,
 *                          high bit set 1 to mask
 *              12..31   -  <UNKNOWN>
 *
 *     dword2:   0..31   -  <UNKNOWN>
 */

static int instr_emit_fetch(struct ir_instruction *instr,
		uint32_t *dwords, uint32_t idx,
		struct ir_shader_info *info)
{
	int reg = 0;
	struct ir_register *dst_reg = instr->regs[reg++];
	struct ir_register *src_reg = instr->regs[reg++];

	dwords[0] = dwords[1] = dwords[2] = 0;

	assert(instr->fetch.constant <= 0xf);

	reg_update_stats(dst_reg, info, true);
	reg_update_stats(src_reg, info, false);

	dwords[0] |= instr_fetch_opc(instr)      << 0;
	dwords[0] |= src_reg->num                << 5;
	dwords[0] |= dst_reg->num                << 12;
	dwords[0] |= instr->fetch.constant       << 20;
	dwords[1] |= reg_fetch_dst_swiz(dst_reg) << 0;

	if (instr->fetch.opc == T_VERTEX) {
		assert(instr->fetch.stride <= 0xff);
		assert(instr->fetch.fmt <= 0x3f);

		dwords[0] |= reg_fetch_src_swiz(src_reg, 1) << 25;

		dwords[1] |= ((instr->fetch.sign == T_SIGNED) ? 1 : 0)
		                                     << 12;
		dwords[1] |= instr->fetch.fmt        << 16;
		dwords[2] |= instr->fetch.stride     << 0;

		/* XXX these bits seems to be always set:
		 */
		dwords[0] |= 0x1                     << 19;
		dwords[0] |= 0x1                     << 24;
		dwords[0] |= 0x1                     << 28;

		/* XXX this seems to always be set, except on the
		 * internal shaders used for GMEM->MEM blits
		 */
		dwords[1] |= 0x1                     << 13;

		/* XXX seems like every FETCH but the first has
		 * this bit set:
		 */
		dwords[1] |= ((idx > 0) ? 0x1 : 0x0) << 30;
		dwords[0] |= ((idx > 0) ? 0x0 : 0x1) << 27;
	} else {
		dwords[0] |= reg_fetch_src_swiz(src_reg, 3) << 26;

		/* XXX not sure about this yet:
		 */
		dwords[1] |= 0x1ffff                 << 12;
		dwords[2] |= 0x1                     << 1;
	}

	return 0;
}

/*
 * ALU instructions:
 */

static int instr_emit_alu(struct ir_instruction *instr, uint32_t *dwords,
		struct ir_shader_info *info)
{
	int reg = 0;
	instr_alu_t *alu = (instr_alu_t *)dwords;
	struct ir_register *dst_reg  = instr->regs[reg++];
	struct ir_register *src1_reg;
	struct ir_register *src2_reg;
	struct ir_register *src3_reg;

	memset(alu, 0, sizeof(*alu));

	/* handle instructions w/ 3 src operands: */
	if (instr->alu.vector_opc == T_MULADDv) {
		/* note: disassembler lists 3rd src first, ie:
		 *   MULADDv Rdst = Rsrc3 + (Rsrc1 * Rsrc2)
		 * which is the reason for this strange ordering.
		 */
		src3_reg = instr->regs[reg++];
	} else {
		src3_reg = NULL;
	}

	src1_reg = instr->regs[reg++];
	src2_reg = instr->regs[reg++];

	reg_update_stats(dst_reg, info, true);
	reg_update_stats(src1_reg, info, false);
	reg_update_stats(src2_reg, info, false);

	assert((dst_reg->flags & ~IR_REG_EXPORT) == 0);
	assert(!dst_reg->swizzle || (strlen(dst_reg->swizzle) == 4));
	assert((src1_reg->flags & IR_REG_EXPORT) == 0);
	assert(!src1_reg->swizzle || (strlen(src1_reg->swizzle) == 4));
	assert((src2_reg->flags & IR_REG_EXPORT) == 0);
	assert(!src2_reg->swizzle || (strlen(src2_reg->swizzle) == 4));

	alu->vector_dest         = dst_reg->num;
	alu->export_data         = !!(dst_reg->flags & IR_REG_EXPORT);
	alu->vector_write_mask   = reg_alu_dst_swiz(dst_reg);
	alu->vector_opc          = instr_vector_opc(instr);

	// TODO predicate case/condition.. need to add to parser

	alu->src2_reg            = src2_reg->num;
	alu->src2_swiz           = reg_alu_src_swiz(src2_reg);
	alu->src2_reg_negate     = !!(src2_reg->flags & IR_REG_NEGATE);
	alu->src2_reg_abs        = !!(src2_reg->flags & IR_REG_ABS);
	alu->src2_sel            = !(src2_reg->flags & IR_REG_CONST);

	alu->src1_reg            = src1_reg->num;
	alu->src1_swiz           = reg_alu_src_swiz(src1_reg);
	alu->src1_reg_negate     = !!(src1_reg->flags & IR_REG_NEGATE);
	alu->src1_reg_abs        = !!(src1_reg->flags & IR_REG_ABS);
	alu->src1_sel            = !(src1_reg->flags & IR_REG_CONST);

	if (instr->alu.scalar_opc) {
		struct ir_register *sdst_reg = instr->regs[reg++];

		reg_update_stats(sdst_reg, info, true);

		assert(sdst_reg->flags == dst_reg->flags);

		if (src3_reg) {
			assert(src3_reg == instr->regs[reg++]);
		} else {
			src3_reg = instr->regs[reg++];
		}

		alu->scalar_dest         = sdst_reg->num;
		alu->scalar_write_mask   = reg_alu_dst_swiz(sdst_reg);
		alu->scalar_opc          = instr_scalar_opc(instr);
	} else {
		/* not sure if this is required, but adreno compiler seems
		 * to always set scalar opc to MAXs if it is not used:
		 */
		alu->scalar_opc = MAXs;
	}

	if (src3_reg) {
		reg_update_stats(src3_reg, info, false);

		alu->src3_reg            = src3_reg->num;
		alu->src3_swiz           = reg_alu_src_swiz(src3_reg);
		alu->src3_reg_negate     = !!(src3_reg->flags & IR_REG_NEGATE);
		alu->src3_reg_abs        = !!(src3_reg->flags & IR_REG_ABS);
		alu->src3_sel            = !(src3_reg->flags & IR_REG_CONST);
	} else {
		/* not sure if this is required, but adreno compiler seems
		 * to always set register bank for 3rd src if unused:
		 */
		alu->src3_sel = 1;
	}

	return 0;
}

static int instr_emit(struct ir_instruction *instr, uint32_t *dwords,
		uint32_t idx, struct ir_shader_info *info)
{
	switch (instr->instr_type) {
	case T_FETCH: return instr_emit_fetch(instr, dwords, idx, info);
	case T_ALU:   return instr_emit_alu(instr, dwords, info);
	}
	return -1;
}


struct ir_register * ir_reg_create(struct ir_instruction *instr,
		int num, const char *swizzle, int flags)
{
	struct ir_register *reg =
			ir_alloc(instr->shader, sizeof(struct ir_register));
	DEBUG_MSG("%x, %d, %s", flags, num, swizzle);
	assert(num <= REG_MASK);
	reg->flags = flags;
	reg->num = num;
	reg->swizzle = ir_strdup(instr->shader, swizzle);
	assert(instr->regs_count < ARRAY_SIZE(instr->regs));
	instr->regs[instr->regs_count++] = reg;
	return reg;
}

static void reg_update_stats(struct ir_register *reg,
		struct ir_shader_info *info, bool dest)
{
	if (!(reg->flags & (IR_REG_CONST|IR_REG_EXPORT))) {
		info->max_reg = max(info->max_reg, reg->num);

		if (dest) {
			info->regs_written |= (1 << reg->num);
		} else if (!(info->regs_written & (1 << reg->num))) {
			/* for registers that haven't been written, they must be an
			 * input register that the thread scheduler (presumably?)
			 * needs to know about:
			 */
			info->max_input_reg = max(info->max_input_reg, reg->num);
		}
	}
}

static uint32_t reg_fetch_src_swiz(struct ir_register *reg, uint32_t n)
{
	uint32_t swiz = 0;
	int i;

	assert(reg->flags == 0);
	assert(reg->swizzle && (strlen(reg->swizzle) == n));

	DEBUG_MSG("fetch src R%d.%s", reg->num, reg->swizzle);

	for (i = n-1; i >= 0; i--) {
		swiz <<= 2;
		switch (reg->swizzle[i]) {
		default:
			ERROR_MSG("invalid fetch src swizzle: %s", reg->swizzle);
		case 'x': swiz |= 0x0; break;
		case 'y': swiz |= 0x1; break;
		case 'z': swiz |= 0x2; break;
		case 'w': swiz |= 0x3; break;
		}
	}

	return swiz;
}

static uint32_t reg_fetch_dst_swiz(struct ir_register *reg)
{
	uint32_t swiz = 0;
	int i;

	assert(reg->flags == 0);
	assert(!reg->swizzle || (strlen(reg->swizzle) == 4));

	DEBUG_MSG("fetch dst R%d.%s", reg->num, reg->swizzle);

	if (reg->swizzle) {
		for (i = 3; i >= 0; i--) {
			swiz <<= 3;
			switch (reg->swizzle[i]) {
			default:
				ERROR_MSG("invalid dst swizzle: %s", reg->swizzle);
			case 'x': swiz |= 0x0; break;
			case 'y': swiz |= 0x1; break;
			case 'z': swiz |= 0x2; break;
			case 'w': swiz |= 0x3; break;
			case '0': swiz |= 0x4; break;
			case '1': swiz |= 0x5; break;
			case '_': swiz |= 0x7; break;
			}
		}
	} else {
		swiz = 0x688;
	}

	return swiz;
}

/* actually, a write-mask */
static uint32_t reg_alu_dst_swiz(struct ir_register *reg)
{
	uint32_t swiz = 0;
	int i;

	assert((reg->flags & ~IR_REG_EXPORT) == 0);
	assert(!reg->swizzle || (strlen(reg->swizzle) == 4));

	DEBUG_MSG("alu dst R%d.%s", reg->num, reg->swizzle);

	if (reg->swizzle) {
		for (i = 3; i >= 0; i--) {
			swiz <<= 1;
			if (reg->swizzle[i] == "xyzw"[i]) {
				swiz |= 0x1;
			} else if (reg->swizzle[i] != '_') {
				ERROR_MSG("invalid dst swizzle: %s", reg->swizzle);
				break;
			}
		}
	} else {
		swiz = 0xf;
	}

	return swiz;
}

static uint32_t reg_alu_src_swiz(struct ir_register *reg)
{
	uint32_t swiz = 0;
	int i;

	assert((reg->flags & IR_REG_EXPORT) == 0);
	assert(!reg->swizzle || (strlen(reg->swizzle) == 4));

	DEBUG_MSG("vector src R%d.%s", reg->num, reg->swizzle);

	if (reg->swizzle) {
		for (i = 3; i >= 0; i--) {
			swiz <<= 2;
			switch (reg->swizzle[i]) {
			default:
				ERROR_MSG("invalid vector src swizzle: %s", reg->swizzle);
			case 'x': swiz |= (0x0 - i) & 0x3; break;
			case 'y': swiz |= (0x1 - i) & 0x3; break;
			case 'z': swiz |= (0x2 - i) & 0x3; break;
			case 'w': swiz |= (0x3 - i) & 0x3; break;
			}
		}
	} else {
		swiz = 0x0;
	}

	return swiz;
}

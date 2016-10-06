/***************************************************************************
 *   Copyright (C) 2015 by David Ung                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/replacements.h>

#include "armv8.h"
#include "arm_disassembler.h"

#include "register.h"
#include <helper/binarybuffer.h>
#include <helper/command.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "armv8_opcodes.h"
#include "target.h"
#include "target_type.h"

#define __unused __attribute__((unused))

static const char * const armv8_state_strings[] = {
	"AArch32", "Thumb", "Jazelle", "ThumbEE", "AArch64",
};

static const struct {
	const char *name;
	unsigned psr;
	/* For user and system modes, these list indices for all registers.
	 * otherwise they're just indices for the shadow registers and SPSR.
	 */
	unsigned short n_indices;
	const uint8_t *indices;
} armv8_mode_data[] = {
	/* These special modes are currently only supported
	 * by ARMv6M and ARMv7M profiles */
	{
		.name = "USR",
		.psr = ARM_MODE_USR,
	},
	{
		.name = "FIQ",
		.psr = ARM_MODE_FIQ,
	},
	{
		.name = "IRQ",
		.psr = ARM_MODE_IRQ,
	},
	{
		.name = "SVC",
		.psr = ARM_MODE_SVC,
	},
	{
		.name = "MON",
		.psr = ARM_MODE_MON,
	},
	{
		.name = "ABT",
		.psr = ARM_MODE_ABT,
	},
	{
		.name = "EL0T",
		.psr = ARMV8_64_EL0T,
	},
	{
		.name = "EL1T",
		.psr = ARMV8_64_EL1T,
	},
	{
		.name = "EL1H",
		.psr = ARMV8_64_EL1H,
	},
	{
		.name = "EL2T",
		.psr = ARMV8_64_EL2T,
	},
	{
		.name = "EL2H",
		.psr = ARMV8_64_EL2H,
	},
	{
		.name = "EL3T",
		.psr = ARMV8_64_EL3T,
	},
	{
		.name = "EL3H",
		.psr = ARMV8_64_EL3H,
	},
};

/** Map PSR mode bits to the name of an ARM processor operating mode. */
const char *armv8_mode_name(unsigned psr_mode)
{
	for (unsigned i = 0; i < ARRAY_SIZE(armv8_mode_data); i++) {
		if (armv8_mode_data[i].psr == psr_mode)
			return armv8_mode_data[i].name;
	}
	LOG_ERROR("unrecognized psr mode: %#02x", psr_mode);
	return "UNRECOGNIZED";
}

int armv8_mode_to_number(enum arm_mode mode)
{
	switch (mode) {
		case ARM_MODE_ANY:
		/* map MODE_ANY to user mode */
		case ARM_MODE_USR:
			return 0;
		case ARM_MODE_FIQ:
			return 1;
		case ARM_MODE_IRQ:
			return 2;
		case ARM_MODE_SVC:
			return 3;
		case ARM_MODE_ABT:
			return 4;
		case ARM_MODE_UND:
			return 5;
		case ARM_MODE_SYS:
			return 6;
		case ARM_MODE_MON:
			return 7;
		case ARMV8_64_EL0T:
			return 8;
		case ARMV8_64_EL1T:
			return 9;
		case ARMV8_64_EL1H:
			return 10;
		case ARMV8_64_EL2T:
			return 11;
		case ARMV8_64_EL2H:
			return 12;
		case ARMV8_64_EL3T:
			return 13;
		case ARMV8_64_EL3H:
			return 14;

		default:
			LOG_ERROR("invalid mode value encountered %d", mode);
			return -1;
	}
}

static int armv8_read_reg(struct armv8_common *armv8, int regnum, uint64_t *regval)
{
	struct arm_dpm *dpm = &armv8->dpm;
	int retval;
	uint32_t value;
	uint64_t value_64;

	switch (regnum) {
	case 0 ... 30:
		retval = dpm->instr_read_data_dcc_64(dpm,
				ARMV8_MSR_GP(SYSTEM_DBG_DBGDTR_EL0, regnum), &value_64);
		break;
	case ARMV8_SP:
		retval = dpm->instr_read_data_r0_64(dpm,
				ARMV8_MOVFSP_64(0), &value_64);
		break;
	case ARMV8_PC:
		retval = dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS_DLR(0), &value_64);
		break;
	case ARMV8_xPSR:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS_DSPSR(0), &value);
		value_64 = value;
		break;
	case ARMV8_ELR_EL1:
		retval = dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS(SYSTEM_ELR_EL1, 0), &value_64);
		break;
	case ARMV8_ELR_EL2:
		retval = dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS(SYSTEM_ELR_EL2, 0), &value_64);
		break;
	case ARMV8_ELR_EL3:
		retval = dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS(SYSTEM_ELR_EL3, 0), &value_64);
		break;
	case ARMV8_ESR_EL1:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_ESR_EL1, 0), &value);
		value_64 = value;
		break;
	case ARMV8_ESR_EL2:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_ESR_EL2, 0), &value);
		value_64 = value;
		break;
	case ARMV8_ESR_EL3:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_ESR_EL3, 0), &value);
		value_64 = value;
		break;
	case ARMV8_SPSR_EL1:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_SPSR_EL1, 0), &value);
		value_64 = value;
		break;
	case ARMV8_SPSR_EL2:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_SPSR_EL2, 0), &value);
		value_64 = value;
		break;
	case ARMV8_SPSR_EL3:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_SPSR_EL3, 0), &value);
		value_64 = value;
		break;
	default:
		retval = ERROR_FAIL;
		break;
	}

	if (retval == ERROR_OK && regval != NULL)
		*regval = value_64;

	return retval;
}

static int armv8_write_reg(struct armv8_common *armv8, int regnum, uint64_t value_64)
{
	struct arm_dpm *dpm = &armv8->dpm;
	int retval;
	uint32_t value;

	switch (regnum) {
	case 0 ... 30:
		retval = dpm->instr_write_data_dcc_64(dpm,
			ARMV8_MRS(SYSTEM_DBG_DBGDTR_EL0, regnum),
			value_64);
		break;
	case ARMV8_SP:
		retval = dpm->instr_write_data_r0_64(dpm,
			ARMV8_MOVTSP_64(0),
			value_64);
		break;
	case ARMV8_PC:
		retval = dpm->instr_write_data_r0_64(dpm,
			ARMV8_MSR_DLR(0),
			value_64);
		break;
	case ARMV8_xPSR:
		value = value_64;
		retval = dpm->instr_write_data_r0(dpm,
			ARMV8_MSR_DSPSR(0),
			value);
		break;
	/* registers clobbered by taking exception in debug state */
	case ARMV8_ELR_EL1:
		retval = dpm->instr_write_data_r0_64(dpm,
				ARMV8_MSR_GP(SYSTEM_ELR_EL1, 0), value_64);
		break;
	case ARMV8_ELR_EL2:
		retval = dpm->instr_write_data_r0_64(dpm,
				ARMV8_MSR_GP(SYSTEM_ELR_EL2, 0), value_64);
		break;
	case ARMV8_ELR_EL3:
		retval = dpm->instr_write_data_r0_64(dpm,
				ARMV8_MSR_GP(SYSTEM_ELR_EL3, 0), value_64);
		break;
	case ARMV8_ESR_EL1:
		value = value_64;
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP(SYSTEM_ESR_EL1, 0), value);
		break;
	case ARMV8_ESR_EL2:
		value = value_64;
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP(SYSTEM_ESR_EL2, 0), value);
		break;
	case ARMV8_ESR_EL3:
		value = value_64;
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP(SYSTEM_ESR_EL3, 0), value);
		break;
	case ARMV8_SPSR_EL1:
		value = value_64;
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP(SYSTEM_SPSR_EL1, 0), value);
		break;
	case ARMV8_SPSR_EL2:
		value = value_64;
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP(SYSTEM_SPSR_EL2, 0), value);
		break;
	case ARMV8_SPSR_EL3:
		value = value_64;
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP(SYSTEM_SPSR_EL3, 0), value);
		break;
	default:
		retval = ERROR_FAIL;
		break;
	}

	return retval;
}

static int armv8_read_reg32(struct armv8_common *armv8, int regnum, uint64_t *regval)
{
	struct arm_dpm *dpm = &armv8->dpm;
	uint32_t value = 0;
	int retval;

	switch (regnum) {
	case ARMV8_R0 ... ARMV8_R14:
		/* return via DCC:  "MCR p14, 0, Rnum, c0, c5, 0" */
		retval = dpm->instr_read_data_dcc(dpm,
			ARMV4_5_MCR(14, 0, regnum, 0, 5, 0),
			&value);
		break;
	case ARMV8_SP:
		retval = dpm->instr_read_data_dcc(dpm,
			ARMV4_5_MCR(14, 0, 13, 0, 5, 0),
			&value);
		break;
	case ARMV8_PC:
		retval = dpm->instr_read_data_r0(dpm,
			ARMV8_MRC_DLR(0),
			&value);
		break;
	case ARMV8_xPSR:
		retval = dpm->instr_read_data_r0(dpm,
			ARMV8_MRC_DSPSR(0),
			&value);
		break;
	case ARMV8_ELR_EL1: /* mapped to LR_svc */
		retval = dpm->instr_read_data_dcc(dpm,
				ARMV4_5_MCR(14, 0, 14, 0, 5, 0),
				&value);
		break;
	case ARMV8_ELR_EL2: /* mapped to ELR_hyp */
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS_T1(0, 14, 0, 1),
				&value);
		break;
	case ARMV8_ELR_EL3: /* mapped to LR_mon */
		retval = dpm->instr_read_data_dcc(dpm,
				ARMV4_5_MCR(14, 0, 14, 0, 5, 0),
				&value);
		break;
	case ARMV8_ESR_EL1: /* mapped to DFSR */
		retval = dpm->instr_read_data_r0(dpm,
				ARMV4_5_MRC(15, 0, 0, 5, 0, 0),
				&value);
		break;
	case ARMV8_ESR_EL2: /* mapped to HSR */
		retval = dpm->instr_read_data_r0(dpm,
				ARMV4_5_MRC(15, 4, 0, 5, 2, 0),
				&value);
		break;
	case ARMV8_ESR_EL3: /* FIXME: no equivalent in aarch32? */
		retval = ERROR_FAIL;
		break;
	case ARMV8_SPSR_EL1: /* mapped to SPSR_svc */
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS_xPSR_T1(1, 0),
				&value);
		break;
	case ARMV8_SPSR_EL2: /* mapped to SPSR_hyp */
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS_xPSR_T1(1, 0),
				&value);
		break;
	case ARMV8_SPSR_EL3: /* mapped to SPSR_mon */
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS_xPSR_T1(1, 0),
				&value);
		break;
	default:
		retval = ERROR_FAIL;
		break;
	}

	if (retval == ERROR_OK && regval != NULL)
		*regval = value;

	return retval;
}

static int armv8_write_reg32(struct armv8_common *armv8, int regnum, uint64_t value)
{
	struct arm_dpm *dpm = &armv8->dpm;
	int retval;

	switch (regnum) {
	case ARMV8_R0 ... ARMV8_R14:
		/* load register from DCC:  "MRC p14, 0, Rnum, c0, c5, 0" */
		retval = dpm->instr_write_data_dcc(dpm,
				ARMV4_5_MRC(14, 0, regnum, 0, 5, 0), value);
		break;
	case ARMV8_SP:
		retval = dpm->instr_write_data_dcc(dpm,
			ARMV4_5_MRC(14, 0, 13, 0, 5, 0),
			value);
			break;
	case ARMV8_PC:/* PC
		 * read r0 from DCC; then "MOV pc, r0" */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MCR_DLR(0), value);
		break;
	case ARMV8_xPSR: /* CPSR */
		/* read r0 from DCC, then "MCR r0, DSPSR" */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MCR_DSPSR(0), value);
		break;
	case ARMV8_ELR_EL1: /* mapped to LR_svc */
		retval = dpm->instr_write_data_dcc(dpm,
				ARMV4_5_MRC(14, 0, 14, 0, 5, 0),
				value);
		break;
	case ARMV8_ELR_EL2: /* mapped to ELR_hyp */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP_T1(0, 14, 0, 1),
				value);
		break;
	case ARMV8_ELR_EL3: /* mapped to LR_mon */
		retval = dpm->instr_write_data_dcc(dpm,
				ARMV4_5_MRC(14, 0, 14, 0, 5, 0),
				value);
		break;
	case ARMV8_ESR_EL1: /* mapped to DFSR */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV4_5_MCR(15, 0, 0, 5, 0, 0),
				value);
		break;
	case ARMV8_ESR_EL2: /* mapped to HSR */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV4_5_MCR(15, 4, 0, 5, 2, 0),
				value);
		break;
	case ARMV8_ESR_EL3: /* FIXME: no equivalent in aarch32? */
		retval = ERROR_FAIL;
		break;
	case ARMV8_SPSR_EL1: /* mapped to SPSR_svc */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP_xPSR_T1(1, 0, 15),
				value);
		break;
	case ARMV8_SPSR_EL2: /* mapped to SPSR_hyp */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP_xPSR_T1(1, 0, 15),
				value);
		break;
	case ARMV8_SPSR_EL3: /* mapped to SPSR_mon */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV8_MSR_GP_xPSR_T1(1, 0, 15),
				value);
		break;
	default:
		retval = ERROR_FAIL;
		break;
	}

	return retval;

}

void armv8_select_reg_access(struct armv8_common *armv8, bool is_aarch64)
{
	if (is_aarch64) {
		armv8->read_reg_u64 = armv8_read_reg;
		armv8->write_reg_u64 = armv8_write_reg;
	} else {
		armv8->read_reg_u64 = armv8_read_reg32;
		armv8->write_reg_u64 = armv8_write_reg32;
	}
}

/*  retrieve core id cluster id  */
int armv8_read_mpidr(struct armv8_common *armv8)
{
	int retval = ERROR_FAIL;
	struct arm_dpm *dpm = armv8->arm.dpm;
	uint32_t mpidr;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;

	retval = dpm->instr_read_data_r0(dpm, armv8_opcode(armv8, READ_REG_MPIDR), &mpidr);
	if (retval != ERROR_OK)
		goto done;
	if (mpidr & 1<<31) {
		armv8->multi_processor_system = (mpidr >> 30) & 1;
		armv8->cluster_id = (mpidr >> 8) & 0xf;
		armv8->cpu_id = mpidr & 0x3;
		LOG_INFO("%s cluster %x core %x %s", target_name(armv8->arm.target),
			armv8->cluster_id,
			armv8->cpu_id,
			armv8->multi_processor_system == 0 ? "multi core" : "mono core");

	} else
		LOG_ERROR("mpdir not in multiprocessor format");

done:
	dpm->finish(dpm);
	return retval;
}

/**
 * Configures host-side ARM records to reflect the specified CPSR.
 * Later, code can use arm_reg_current() to map register numbers
 * according to how they are exposed by this mode.
 */
void armv8_set_cpsr(struct arm *arm, uint32_t cpsr)
{
	uint32_t mode = cpsr & 0x1F;

	/* NOTE:  this may be called very early, before the register
	 * cache is set up.  We can't defend against many errors, in
	 * particular against CPSRs that aren't valid *here* ...
	 */
	if (arm->cpsr) {
		buf_set_u32(arm->cpsr->value, 0, 32, cpsr);
		arm->cpsr->valid = 1;
		arm->cpsr->dirty = 0;
	}

	/* Older ARMs won't have the J bit */
	enum arm_state state = 0xFF;

	if (((cpsr & 0x10) >> 4) == 0) {
		state = ARM_STATE_AARCH64;
	} else {
		if (cpsr & (1 << 5)) {	/* T */
			if (cpsr & (1 << 24)) { /* J */
				LOG_WARNING("ThumbEE -- incomplete support");
				state = ARM_STATE_THUMB_EE;
			} else
				state = ARM_STATE_THUMB;
		} else {
			if (cpsr & (1 << 24)) { /* J */
				LOG_ERROR("Jazelle state handling is BROKEN!");
				state = ARM_STATE_JAZELLE;
			} else
				state = ARM_STATE_ARM;
		}
	}
	arm->core_state = state;
	if (arm->core_state == ARM_STATE_AARCH64)
		arm->core_mode = (mode << 4) | 0xf;
	else
		arm->core_mode = mode;

	LOG_DEBUG("set CPSR %#8.8x: %s mode, %s state", (unsigned) cpsr,
		armv8_mode_name(arm->core_mode),
		armv8_state_strings[arm->core_state]);
}

static void armv8_show_fault_registers32(struct armv8_common *armv8)
{
	uint32_t dfsr, ifsr, dfar, ifar;
	struct arm_dpm *dpm = armv8->arm.dpm;
	int retval;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return;

	/* ARMV4_5_MRC(cpnum, op1, r0, CRn, CRm, op2) */

	/* c5/c0 - {data, instruction} fault status registers */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 5, 0, 0),
			&dfsr);
	if (retval != ERROR_OK)
		goto done;

	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 5, 0, 1),
			&ifsr);
	if (retval != ERROR_OK)
		goto done;

	/* c6/c0 - {data, instruction} fault address registers */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 6, 0, 0),
			&dfar);
	if (retval != ERROR_OK)
		goto done;

	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 6, 0, 2),
			&ifar);
	if (retval != ERROR_OK)
		goto done;

	LOG_USER("Data fault registers        DFSR: %8.8" PRIx32
		", DFAR: %8.8" PRIx32, dfsr, dfar);
	LOG_USER("Instruction fault registers IFSR: %8.8" PRIx32
		", IFAR: %8.8" PRIx32, ifsr, ifar);

done:
	/* (void) */ dpm->finish(dpm);
}

static void armv8_show_fault_registers(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);

	if (armv8->arm.core_state != ARM_STATE_AARCH64)
		armv8_show_fault_registers32(armv8);
}

static uint8_t armv8_pa_size(uint32_t ps)
{
	uint8_t ret = 0;
	switch (ps) {
		case 0:
			ret = 32;
			break;
		case 1:
			ret = 36;
			break;
		case 2:
			ret = 40;
			break;
		case 3:
			ret = 42;
			break;
		case 4:
			ret = 44;
			break;
		case 5:
			ret = 48;
			break;
		default:
			LOG_INFO("Unknow physicall address size");
			break;
	}
	return ret;
}

static __unused int armv8_read_ttbcr32(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
	uint32_t ttbcr, ttbcr_n;
	int retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;
	/*  MRC p15,0,<Rt>,c2,c0,2 ; Read CP15 Translation Table Base Control Register*/
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(15, 0, 0, 2, 0, 2),
			&ttbcr);
	if (retval != ERROR_OK)
		goto done;

	LOG_DEBUG("ttbcr %" PRIx32, ttbcr);

	ttbcr_n = ttbcr & 0x7;
	armv8->armv8_mmu.ttbcr = ttbcr;

	/*
	 * ARM Architecture Reference Manual (ARMv7-A and ARMv7-Redition),
	 * document # ARM DDI 0406C
	 */
	armv8->armv8_mmu.ttbr_range[0]  = 0xffffffff >> ttbcr_n;
	armv8->armv8_mmu.ttbr_range[1] = 0xffffffff;
	armv8->armv8_mmu.ttbr_mask[0] = 0xffffffff << (14 - ttbcr_n);
	armv8->armv8_mmu.ttbr_mask[1] = 0xffffffff << 14;

	LOG_DEBUG("ttbr1 %s, ttbr0_mask %" PRIx32 " ttbr1_mask %" PRIx32,
		  (ttbcr_n != 0) ? "used" : "not used",
		  armv8->armv8_mmu.ttbr_mask[0],
		  armv8->armv8_mmu.ttbr_mask[1]);

done:
	dpm->finish(dpm);
	return retval;
}

static __unused int armv8_read_ttbcr(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm_dpm *dpm = armv8->arm.dpm;
	struct arm *arm = &armv8->arm;
	uint32_t ttbcr;
	uint64_t ttbcr_64;

	int retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;

	/* claaer ttrr1_used and ttbr0_mask */
	memset(&armv8->armv8_mmu.ttbr1_used, 0, sizeof(armv8->armv8_mmu.ttbr1_used));
	memset(&armv8->armv8_mmu.ttbr0_mask, 0, sizeof(armv8->armv8_mmu.ttbr0_mask));

	switch (armv8_curel_from_core_mode(arm->core_mode)) {
	case SYSTEM_CUREL_EL3:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_TCR_EL3, 0),
				&ttbcr);
		retval += dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS(SYSTEM_TTBR0_EL3, 0),
				&armv8->ttbr_base);
		if (retval != ERROR_OK)
			goto done;
		armv8->va_size = 64 - (ttbcr & 0x3F);
		armv8->pa_size = armv8_pa_size((ttbcr >> 16) & 7);
		armv8->page_size = (ttbcr >> 14) & 3;
		break;
	case SYSTEM_CUREL_EL2:
		retval = dpm->instr_read_data_r0(dpm,
				ARMV8_MRS(SYSTEM_TCR_EL2, 0),
				&ttbcr);
		retval += dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS(SYSTEM_TTBR0_EL2, 0),
				&armv8->ttbr_base);
		if (retval != ERROR_OK)
			goto done;
		armv8->va_size = 64 - (ttbcr & 0x3F);
		armv8->pa_size = armv8_pa_size((ttbcr >> 16) & 7);
		armv8->page_size = (ttbcr >> 14) & 3;
		break;
	case SYSTEM_CUREL_EL0:
	case SYSTEM_CUREL_EL1:
		retval = dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS(SYSTEM_TCR_EL1, 0),
				&ttbcr_64);
		armv8->va_size = 64 - (ttbcr_64 & 0x3F);
		armv8->pa_size = armv8_pa_size((ttbcr_64 >> 32) & 7);
		armv8->page_size = (ttbcr_64 >> 14) & 3;
		armv8->armv8_mmu.ttbr1_used = (((ttbcr_64 >> 16) & 0x3F) != 0) ? 1 : 0;
		armv8->armv8_mmu.ttbr0_mask  = 0x0000FFFFFFFFFFFF;
		retval += dpm->instr_read_data_r0_64(dpm,
				ARMV8_MRS(SYSTEM_TTBR0_EL1 | (armv8->armv8_mmu.ttbr1_used), 0),
				&armv8->ttbr_base);
		if (retval != ERROR_OK)
			goto done;
		break;
	default:
		LOG_ERROR("unknow core state");
		retval = ERROR_FAIL;
		break;
	}
	if (retval != ERROR_OK)
		goto done;

	if (armv8->armv8_mmu.ttbr1_used == 1)
		LOG_INFO("TTBR0 access above %" PRIx64, (uint64_t)(armv8->armv8_mmu.ttbr0_mask));

done:
	dpm->finish(dpm);
	return retval;
}

/*  method adapted to cortex A : reused arm v4 v5 method*/
int armv8_mmu_translate_va(struct target *target,  target_addr_t va, target_addr_t *val)
{
	return ERROR_OK;
}

/*  V8 method VA TO PA  */
int armv8_mmu_translate_va_pa(struct target *target, target_addr_t va,
	target_addr_t *val, int meminfo)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = target_to_arm(target);
	struct arm_dpm *dpm = &armv8->dpm;
	uint32_t retval;
	uint32_t instr = 0;
	uint64_t par;

	static const char * const shared_name[] = {
			"Non-", "UNDEFINED ", "Outer ", "Inner "
	};

	static const char * const secure_name[] = {
			"Secure", "Not Secure"
	};

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return retval;

	switch (armv8_curel_from_core_mode(arm->core_mode)) {
	case SYSTEM_CUREL_EL0:
		instr = ARMV8_SYS(SYSTEM_ATS12E0R, 0);
		/* can only execute instruction at EL2 */
		dpmv8_modeswitch(dpm, ARMV8_64_EL2T);
		break;
	case SYSTEM_CUREL_EL1:
		instr = ARMV8_SYS(SYSTEM_ATS12E1R, 0);
		/* can only execute instruction at EL2 */
		dpmv8_modeswitch(dpm, ARMV8_64_EL2T);
		break;
	case SYSTEM_CUREL_EL2:
		instr = ARMV8_SYS(SYSTEM_ATS1E2R, 0);
		break;
	case SYSTEM_CUREL_EL3:
		instr = ARMV8_SYS(SYSTEM_ATS1E3R, 0);
		break;

	default:
		break;
	};

	/* write VA to R0 and execute translation instruction */
	retval = dpm->instr_write_data_r0_64(dpm, instr, (uint64_t)va);
	/* read result from PAR_EL1 */
	if (retval == ERROR_OK)
		retval = dpm->instr_read_data_r0_64(dpm, ARMV8_MRS(SYSTEM_PAR_EL1, 0), &par);

	dpm->finish(dpm);

	/* switch back to saved PE mode */
	dpmv8_modeswitch(dpm, ARM_MODE_ANY);

	if (retval != ERROR_OK)
		return retval;

	if (par & 1) {
		LOG_ERROR("Address translation failed at stage %i, FST=%x, PTW=%i",
				((int)(par >> 9) & 1)+1, (int)(par >> 1) & 0x3f, (int)(par >> 8) & 1);

		*val = 0;
		retval = ERROR_FAIL;
	} else {
		*val = (par & 0xFFFFFFFFF000UL) | (va & 0xFFF);
		if (meminfo) {
			int SH = (par >> 7) & 3;
			int NS = (par >> 9) & 1;
			int ATTR = (par >> 56) & 0xFF;

			char *memtype = (ATTR & 0xF0) == 0 ? "Device Memory" : "Normal Memory";

			LOG_USER("%sshareable, %s",
					shared_name[SH], secure_name[NS]);
			LOG_USER("%s", memtype);
		}
	}

	return retval;
}

int armv8_handle_cache_info_command(struct command_context *cmd_ctx,
	struct armv8_cache_common *armv8_cache)
{
	if (armv8_cache->info == -1) {
		command_print(cmd_ctx, "cache not yet identified");
		return ERROR_OK;
	}

	if (armv8_cache->display_cache_info)
		armv8_cache->display_cache_info(cmd_ctx, armv8_cache);
	return ERROR_OK;
}

int armv8_init_arch_info(struct target *target, struct armv8_common *armv8)
{
	struct arm *arm = &armv8->arm;
	arm->arch_info = armv8;
	target->arch_info = &armv8->arm;
	/*  target is useful in all function arm v4 5 compatible */
	armv8->arm.target = target;
	armv8->arm.common_magic = ARM_COMMON_MAGIC;
	armv8->common_magic = ARMV8_COMMON_MAGIC;

	armv8->armv8_mmu.armv8_cache.l2_cache = NULL;
	armv8->armv8_mmu.armv8_cache.info = -1;
	armv8->armv8_mmu.armv8_cache.flush_all_data_cache = NULL;
	armv8->armv8_mmu.armv8_cache.display_cache_info = NULL;
	return ERROR_OK;
}

int armv8_aarch64_state(struct target *target)
{
	struct arm *arm = target_to_arm(target);

	if (arm->common_magic != ARM_COMMON_MAGIC) {
		LOG_ERROR("BUG: called for a non-ARM target");
		return ERROR_FAIL;
	}

	LOG_USER("target halted in %s state due to %s, current mode: %s\n"
		"cpsr: 0x%8.8" PRIx32 " pc: 0x%" PRIx64 "%s",
		armv8_state_strings[arm->core_state],
		debug_reason_name(target),
		armv8_mode_name(arm->core_mode),
		buf_get_u32(arm->cpsr->value, 0, 32),
		buf_get_u64(arm->pc->value, 0, 64),
		arm->is_semihosting ? ", semihosting" : "");

	return ERROR_OK;
}

int armv8_arch_state(struct target *target)
{
	static const char * const state[] = {
		"disabled", "enabled"
	};

	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;

	if (armv8->common_magic != ARMV8_COMMON_MAGIC) {
		LOG_ERROR("BUG: called for a non-Armv8 target");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (arm->core_state == ARM_STATE_AARCH64)
		armv8_aarch64_state(target);
	else
		arm_arch_state(target);

	LOG_USER("MMU: %s, D-Cache: %s, I-Cache: %s",
		state[armv8->armv8_mmu.mmu_enabled],
		state[armv8->armv8_mmu.armv8_cache.d_u_cache_enabled],
		state[armv8->armv8_mmu.armv8_cache.i_cache_enabled]);

	if (arm->core_mode == ARM_MODE_ABT)
		armv8_show_fault_registers(target);

	if (target->debug_reason == DBG_REASON_WATCHPOINT)
		LOG_USER("Watchpoint triggered at PC %#08x",
			(unsigned) armv8->dpm.wp_pc);

	return ERROR_OK;
}

static const struct {
	unsigned id;
	const char *name;
	unsigned bits;
	enum arm_mode mode;
	enum reg_type type;
	const char *group;
	const char *feature;
} armv8_regs[] = {
	{ ARMV8_R0,  "x0",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R1,  "x1",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R2,  "x2",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R3,  "x3",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R4,  "x4",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R5,  "x5",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R6,  "x6",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R7,  "x7",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R8,  "x8",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R9,  "x9",  64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R10, "x10", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R11, "x11", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R12, "x12", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R13, "x13", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R14, "x14", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R15, "x15", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R16, "x16", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R17, "x17", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R18, "x18", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R19, "x19", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R20, "x20", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R21, "x21", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R22, "x22", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R23, "x23", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R24, "x24", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R25, "x25", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R26, "x26", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R27, "x27", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R28, "x28", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R29, "x29", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_R30, "x30", 64, ARM_MODE_ANY, REG_TYPE_UINT64, "general", "org.gnu.gdb.aarch64.core" },

	{ ARMV8_SP, "sp", 64, ARM_MODE_ANY, REG_TYPE_DATA_PTR, "general", "org.gnu.gdb.aarch64.core" },
	{ ARMV8_PC,  "pc", 64, ARM_MODE_ANY, REG_TYPE_CODE_PTR, "general", "org.gnu.gdb.aarch64.core" },

	{ ARMV8_xPSR, "CPSR", 32, ARM_MODE_ANY, REG_TYPE_UINT32, "general", "org.gnu.gdb.aarch64.core" },

	{ ARMV8_ELR_EL1, "ELR_EL1", 64, ARMV8_64_EL1H, REG_TYPE_CODE_PTR, "banked", "net.sourceforge.openocd.banked" },
	{ ARMV8_ESR_EL1, "ESR_EL1", 32, ARMV8_64_EL1H, REG_TYPE_UINT32, "banked", "net.sourceforge.openocd.banked" },
	{ ARMV8_SPSR_EL1, "SPSR_EL1", 32, ARMV8_64_EL1H, REG_TYPE_UINT32, "banked", "net.sourceforge.openocd.banked" },

	{ ARMV8_ELR_EL2, "ELR_EL2", 64, ARMV8_64_EL2H, REG_TYPE_CODE_PTR, "banked", "net.sourceforge.openocd.banked" },
	{ ARMV8_ESR_EL2, "ESR_EL2", 32, ARMV8_64_EL2H, REG_TYPE_UINT32, "banked", "net.sourceforge.openocd.banked" },
	{ ARMV8_SPSR_EL2, "SPSR_EL2", 32, ARMV8_64_EL2H, REG_TYPE_UINT32, "banked", "net.sourceforge.openocd.banked" },

	{ ARMV8_ELR_EL3, "ELR_EL3", 64, ARMV8_64_EL3H, REG_TYPE_CODE_PTR, "banked", "net.sourceforge.openocd.banked" },
	{ ARMV8_ESR_EL3, "ESR_EL3", 32, ARMV8_64_EL3H, REG_TYPE_UINT32, "banked", "net.sourceforge.openocd.banked" },
	{ ARMV8_SPSR_EL3, "SPSR_EL3", 32, ARMV8_64_EL3H, REG_TYPE_UINT32, "banked", "net.sourceforge.openocd.banked" },
};

#define ARMV8_NUM_REGS ARRAY_SIZE(armv8_regs)


static int armv8_get_core_reg(struct reg *reg)
{
	int retval;
	struct arm_reg *armv8_reg = reg->arch_info;
	struct target *target = armv8_reg->target;
	struct arm *arm = target_to_arm(target);

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	retval = arm->read_core_reg(target, reg, armv8_reg->num, arm->core_mode);

	return retval;
}

static int armv8_set_core_reg(struct reg *reg, uint8_t *buf)
{
	struct arm_reg *armv8_reg = reg->arch_info;
	struct target *target = armv8_reg->target;
	struct arm *arm = target_to_arm(target);
	uint64_t value = buf_get_u64(buf, 0, 64);

	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;

	if (reg == arm->cpsr) {
		armv8_set_cpsr(arm, (uint32_t)value);
	} else {
		buf_set_u64(reg->value, 0, 64, value);
		reg->valid = 1;
	}

	reg->dirty = 1;

	return ERROR_OK;
}

static const struct reg_arch_type armv8_reg_type = {
	.get = armv8_get_core_reg,
	.set = armv8_set_core_reg,
};

/** Builds cache of architecturally defined registers.  */
struct reg_cache *armv8_build_reg_cache(struct target *target)
{
	struct armv8_common *armv8 = target_to_armv8(target);
	struct arm *arm = &armv8->arm;
	int num_regs = ARMV8_NUM_REGS;
	struct reg_cache **cache_p = register_get_last_cache_p(&target->reg_cache);
	struct reg_cache *cache = malloc(sizeof(struct reg_cache));
	struct reg *reg_list = calloc(num_regs, sizeof(struct reg));
	struct arm_reg *arch_info = calloc(num_regs, sizeof(struct arm_reg));
	struct reg_feature *feature;
	int i;

	/* Build the process context cache */
	cache->name = "arm v8 registers";
	cache->next = NULL;
	cache->reg_list = reg_list;
	cache->num_regs = num_regs;
	(*cache_p) = cache;

	for (i = 0; i < num_regs; i++) {
		arch_info[i].num = armv8_regs[i].id;
		arch_info[i].mode = armv8_regs[i].mode;
		arch_info[i].target = target;
		arch_info[i].arm = arm;

		reg_list[i].name = armv8_regs[i].name;
		reg_list[i].size = armv8_regs[i].bits;
		reg_list[i].value = calloc(1, 8);
		reg_list[i].dirty = 0;
		reg_list[i].valid = 0;
		reg_list[i].type = &armv8_reg_type;
		reg_list[i].arch_info = &arch_info[i];

		reg_list[i].group = armv8_regs[i].group;
		reg_list[i].number = i;
		reg_list[i].exist = true;
		reg_list[i].caller_save = true;	/* gdb defaults to true */

		feature = calloc(1, sizeof(struct reg_feature));
		if (feature) {
			feature->name = armv8_regs[i].feature;
			reg_list[i].feature = feature;
		} else
			LOG_ERROR("unable to allocate feature list");

		reg_list[i].reg_data_type = calloc(1, sizeof(struct reg_data_type));
		if (reg_list[i].reg_data_type)
			reg_list[i].reg_data_type->type = armv8_regs[i].type;
		else
			LOG_ERROR("unable to allocate reg type list");
	}

	arm->cpsr = reg_list + ARMV8_xPSR;
	arm->pc = reg_list + ARMV8_PC;
	arm->core_cache = cache;

	return cache;
}

struct reg *armv8_reg_current(struct arm *arm, unsigned regnum)
{
	struct reg *r;

	if (regnum > (ARMV8_LAST_REG - 1))
		return NULL;

	r = arm->core_cache->reg_list + regnum;
	return r;
}

const struct command_registration armv8_command_handlers[] = {
	{
		.chain = dap_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};


int armv8_get_gdb_reg_list(struct target *target,
	struct reg **reg_list[], int *reg_list_size,
	enum target_register_class reg_class)
{
	struct arm *arm = target_to_arm(target);
	int i;

	switch (reg_class) {
	case REG_CLASS_GENERAL:
		*reg_list_size = ARMV8_ELR_EL1;
		*reg_list = malloc(sizeof(struct reg *) * (*reg_list_size));

		for (i = 0; i < *reg_list_size; i++)
				(*reg_list)[i] = armv8_reg_current(arm, i);

		return ERROR_OK;
	case REG_CLASS_ALL:
		*reg_list_size = ARMV8_LAST_REG;
		*reg_list = malloc(sizeof(struct reg *) * (*reg_list_size));

		for (i = 0; i < *reg_list_size; i++)
				(*reg_list)[i] = armv8_reg_current(arm, i);

		return ERROR_OK;

	default:
		LOG_ERROR("not a valid register class type in query.");
		return ERROR_FAIL;
		break;
	}
}
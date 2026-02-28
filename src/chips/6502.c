#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include "../types.h"
#include "../bitmath.h"
#include "../systems/system.h"
#include "2C02.h"
#include "6502.h"
#include "../systems/famicom.h"
#include "../systems/apple1.h"
#include "../systems/sst.h"

#define SET_BIT(b,i) (b | 1 << i)
#define CLEAR_BIT(b,i) (b & ~(1 << i))
#define GET_BIT(b,i) (b>>i) & 1
#define MEM_READ(ad) 			mmap_6502(system, ad, 0, false)
#define MEM_WRITE(ad, v) 	mmap_6502(system, ad, v, true)
#define PUSH_STACK(v) 		MEM_WRITE(0x100 + cpu->reg[reg_sp], v); cpu->reg[reg_sp] -= 1;
#define PULL_STACK( )			({cpu->reg[reg_sp] += 1; MEM_READ(0x100 + cpu->reg[reg_sp]);})
#define GET_P(f) 						({GET_BIT(cpu->reg[reg_p], f);})

void set_p ( Cpu_6502* cpu, enum flag f, bool value )
{
	if (value) {
		cpu->reg[reg_p] = SET_BIT(cpu->reg[reg_p], f);
	} else {
		cpu->reg[reg_p] = CLEAR_BIT(cpu->reg[reg_p], f);
	}
}

byte mmap_6502 (System system, word addr, byte value, bool write)
{
	switch (system.s) {
	case famicom_system:
		return mmap_famicom(system.h, addr, value, write);
	case apple1_system:
		return mmap_apple1(system.h, addr, value, write);
	case sst_system:
		return mmap_sst(system.h, addr, value, write);
	default:
		return 0;
	}
}

void cpu_reset(Cpu_6502* cpu, System system)
{
	memset(cpu->reg, 0, sizeof(cpu->reg));
	cpu->reg[reg_p] = 0x20; // 00100000 (the unused flag needs to be set)
	cpu->reg[reg_sp] = 0xFD;
	cpu->pc = bytes_to_word(MEM_READ(0xFFFD), MEM_READ(0xFFFC));
	cpu->running = true;
	cpu->current_instruction_name = NULL;
}

byte address (System system, Cpu_6502* cpu, byte oper[2], enum addressing_mode a, bool write, byte value)
{
	word addr_a = bytes_to_word(oper[1], oper[0]);
	word addr_x;
	word addr_y;
	word addr_f;
	byte addr_zp;
	switch (a) {
	case immediate:
		return oper[0];
	case accumulator:
		if (write) {
			cpu->reg[reg_a] = value;
			return 0;
		} else {
			return cpu->reg[reg_a];
		}
		break;
	case zeropage:
		if (write) {
			MEM_WRITE(addr_a % 0x100, value);
			return 0;
		} else {
			return MEM_READ(addr_a % 0x100);
		}
		break;
	case absolute:
		if (write) {
			MEM_WRITE(addr_a, value);
			return 0;
		} else {
			return MEM_READ(addr_a);
		}
		break;
	case zeropage_x:
		addr_zp = (oper[0] + cpu->reg[reg_x]);
		if (write) {
			MEM_WRITE(addr_zp, value);
			return 0;
		} else {
			return MEM_READ(addr_zp);
		}
		break;
	case zeropage_y:
		addr_zp = (oper[0] + cpu->reg[reg_y]);
		if (write) {
			MEM_WRITE(addr_zp, value);
			return 0;
		} else {
			return MEM_READ(addr_zp);
		}
		break;
	case absolute_x:
		addr_f = addr_a + cpu->reg[reg_x];
		if (write) {
			MEM_WRITE(addr_f, value);
			return 0;
		} else {
			return MEM_READ(addr_f);
		}
		break;
	case absolute_y:
		addr_f = addr_a + cpu->reg[reg_y];
		if (write) {
			MEM_WRITE(addr_f, value);
			return 0;
		} else {
			return MEM_READ(addr_f);
		}
		break;
	case zeropage_xi:
		addr_zp = (oper[0] + cpu->reg[reg_x]);
		if (addr_zp != 0xFF) {
			addr_f = bytes_to_word(MEM_READ(addr_zp+1), MEM_READ(addr_zp));
		} else {
			addr_f = bytes_to_word(MEM_READ(0), MEM_READ(addr_zp));
		}
		if (write) {
			MEM_WRITE(addr_f, value);
			return 0;
		} else {
			return MEM_READ(addr_f);
		}
		break;
	case zeropage_yi:
		if (oper[0] != 0xFF) {
			addr_f = bytes_to_word(MEM_READ((oper[0]+1)), MEM_READ(oper[0])) + cpu->reg[reg_y];
		} else {
			addr_f = bytes_to_word(MEM_READ(0), MEM_READ(oper[0])) + cpu->reg[reg_y];
		}
		if (write) {
			MEM_WRITE(addr_f, value);
			return 0;
		} else {
			return MEM_READ(addr_f);
		}
		break;
	default:
		return 0;
	}
}

byte peek(System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	return address(system, cpu, oper, a, false, 0);
}

void poke(System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2], byte value)
{
	address(system, cpu, oper, a, true, value);
}

void instruction (System system, Cpu_6502* cpu, enum operation o, enum register_ r, enum addressing_mode a, enum flag f, byte oper[2], char* name)
{
	cpu->current_instruction_name = name;
	byte value;
	byte value_old;
	int bigvalue;
	word addr;
	word opera = bytes_to_word(oper[1], oper[0]);
	byte low;
	byte high;
	cpu->branch_taken = false;

	switch (o) {
	case no_op:
		break;
	//memory
	case write_mem:
		value = cpu->reg[r];
		poke(system, cpu, a, oper, value);
		break;

	case increment_mem:
		value = peek(system, cpu, a, oper) + 1;
		poke(system, cpu, a, oper, value);
		goto check_flag_nz;
		break;

	case decrement_mem:
		value = peek(system, cpu, a, oper) - 1;
		poke(system, cpu, a, oper, value);
		goto check_flag_nz;
		break;

	case shift_rol:
		value_old = peek(system, cpu, a, oper);
		value = value_old << 1;
		value = set_bit(value, 0, GET_P(carry));
		poke(system, cpu, a, oper, value);
		set_p(cpu, carry, value_old & 0x80);
		goto check_flag_nz;
		break;

	case shift_ror:
		value_old = peek(system, cpu, a, oper);
		value = value_old >> 1;
		value = set_bit(value, 7, GET_P(carry));
		poke(system, cpu, a, oper, value);
		set_p(cpu, carry, value_old & 0x01);
		goto check_flag_nz;
		break;

	case logical_shift_right:
		value = peek(system, cpu, a, oper);
		set_p(cpu, carry, value & 0x01);
		value >>= 1;
		poke(system, cpu, a, oper, value);
		goto check_flag_nz;
		break;

	case arithmetic_shift_left:
		value = peek(system, cpu, a, oper);
		set_p(cpu, carry, value & 0x80);
		value <<= 1;
		poke(system, cpu, a, oper, value);
		goto check_flag_nz;
		break;

	case compare_reg_mem:
		value = cpu->reg[r] - peek(system, cpu, a, oper);
		set_p(cpu, carry, cpu->reg[r] >= value);
		goto check_flag_nz;
		break;

	case compare_bit:
		value = peek(system, cpu, a, oper);
		set_p(cpu, zero, (value & cpu->reg[reg_a]) == 0);
		set_p(cpu, negative, value & 0x80);
		set_p(cpu, overflow, value & 0x40);
		break;

		//register
	case alter_register:
		value = peek(system, cpu, a, oper);
		cpu->reg[r] = value;
		goto check_flag_nz;
		break;

	case add:
		value_old = cpu->reg[reg_a];
		bigvalue = peek(system, cpu, a, oper) + cpu->reg[reg_a] + GET_P(carry);
		value = (byte)bigvalue;
		cpu->reg[reg_a] = value;
		set_p(cpu, carry, 0xFF < bigvalue);
		set_p(cpu, overflow, (value ^ value_old) & (value ^ peek(system, cpu, a, oper)) & 0x80);
		goto check_flag_nz;
		break;

	case subtract:
		value_old = cpu->reg[reg_a];
		value = cpu->reg[reg_a] + ~peek(system, cpu, a, oper) + GET_P(carry);
		bigvalue = cpu->reg[reg_a] + ~peek(system, cpu, a, oper) + GET_P(carry);
		cpu->reg[reg_a] = value;
		set_p(cpu, carry, (0 <= bigvalue));
		set_p(cpu, overflow, (value ^ value_old) & (value ^ ~peek(system, cpu, a, oper)) & 0x80);
		goto check_flag_nz;
		break;

	case increment_reg:
		cpu->reg[r] += 1;
		value = cpu->reg[r];
		goto check_flag_nz;
		break;

	case decrement_reg:
		cpu->reg[r] -= 1;
		value = cpu->reg[r];
		goto check_flag_nz;
		break;

	case and_a:
		value = peek(system, cpu, a, oper) & cpu->reg[reg_a];
		cpu->reg[reg_a] = value;
		goto check_flag_nz;
		break;

	case or_a:
		value = peek(system, cpu, a, oper) | cpu->reg[reg_a];
		cpu->reg[reg_a] = value;
		goto check_flag_nz;
		break;

	case xor_a:
		value = peek(system, cpu, a, oper) ^ cpu->reg[reg_a];
		cpu->reg[reg_a] = value;
		goto check_flag_nz;
		break;

	case transfer_reg_a:
		value = cpu->reg[r];
		cpu->reg[reg_a] = value;
		goto check_flag_nz;
		break;
	case transfer_reg_x:
		value = cpu->reg[r];
		cpu->reg[reg_x] = value;
		goto check_flag_nz;
		break;
	case transfer_reg_sp:
		value = cpu->reg[r];
		cpu->reg[reg_sp] = value;
		break;
	case transfer_reg_y:
		value = cpu->reg[r];
		cpu->reg[reg_y] = value;
		goto check_flag_nz;
		break;

		//branch
        case branch:
		if (a == absolute) {
			addr = opera;
		} else if (a == absolute_indirect) {
			if (oper[0] == 0xFF) {
				addr = bytes_to_word(MEM_READ(bytes_to_word(oper[1],00)), MEM_READ(opera));
			} else {
				addr = bytes_to_word(MEM_READ(opera+1), MEM_READ(opera));
			}
		}
		cpu->pc = addr;
		cpu->branch_taken = true;
		break;

	case branch_jsr:
		addr = cpu->pc + 2;
		PUSH_STACK(get_higher_byte(addr));
	        PUSH_STACK(get_lower_byte(addr));
                cpu->pc = opera;
		cpu->branch_taken = true;
		break;

	case branch_rts:
		low = PULL_STACK();
		high = PULL_STACK();
		addr = bytes_to_word(high, low);
		cpu->pc = addr;
		cpu->branch_taken = false;
		break;

	case branch_rti:
		cpu->reg[reg_p] = PULL_STACK();
		set_p(cpu, unused_flag, true);
		set_p(cpu, break_, false);
		low = PULL_STACK();
		high = PULL_STACK();
		addr = bytes_to_word(high, low);
		cpu->pc = addr;
		cpu->branch_taken = true;
		break;

	case branch_brk:
		addr = bytes_to_word(MEM_READ(0xFFFF), MEM_READ(0xFFFE));
		PUSH_STACK(((cpu->pc + 2) & 0xFF00) >> 8);
		PUSH_STACK((cpu->pc + 2) & 0xFF);
		byte p_old = cpu->reg[reg_p];
		set_p(cpu, break_, true);
		set_p(cpu, interrupt_disable, true);
		PUSH_STACK(cpu->reg[reg_p]);
		cpu->reg[reg_p] = p_old;
		cpu->pc = addr;
		cpu->branch_taken = true;
		break;

	case branch_conditional_flag:
		addr = (int8_t)oper[0] + cpu->pc;
		if (GET_P(f) == 1)
			cpu->pc = addr;
		break;

	case branch_conditional_flag_clear:
		addr = (int8_t)oper[0] + cpu->pc;
		if (GET_P(f) == 0)
			cpu->pc = addr;
		break;

	case push_reg_stack:
		PUSH_STACK(cpu->reg[r]);
		break;

	case pull_reg_stack:
		value = PULL_STACK();
		cpu->reg[r] = value;
		break;

	case instruction_php:
		PUSH_STACK(cpu->reg[reg_p] | 0x30);
		break;

	case instruction_pla:
		value = PULL_STACK();
		cpu->reg[reg_a] = value;
		goto check_flag_nz;
		break;

	case instruction_plp:
		cpu->reg[reg_p] = PULL_STACK();
		break;

	case set_flag:
		set_p(cpu, f, true);
		break;

	case clear_flag:
		set_p(cpu, f, false);
		break;
        }
	return;
check_flag_nz:
	set_p(cpu, negative, value & 0x80);
	set_p(cpu, zero, value == 0);
	return;
}

void nmi(System system, Cpu_6502* cpu)
{
	PUSH_STACK(get_higher_byte(cpu->pc));
	PUSH_STACK(get_lower_byte(cpu->pc));
	set_p(cpu, break_, false);
	PUSH_STACK(cpu->reg[reg_p]);
	cpu->pc = bytes_to_word(MEM_READ(0xFFFB), MEM_READ(0xFFFA));
}

void step(System system, Cpu_6502* cpu, Instruction i, byte oper[2])
{
	cpu->branch_taken = false;
	switch (i.n)
	{
	case LDA:
		instruction(system, cpu, alter_register, reg_a, i.a, 0, oper, "lda");
		break;
	case LDX:
		instruction(system, cpu, alter_register, reg_x, i.a, 0, oper, "ldx");
		break;
	case LDY:
		instruction(system, cpu, alter_register, reg_y, i.a, 0, oper, "ldy");
		break;
	case STA:
		instruction(system, cpu, write_mem, reg_a, i.a, 0, oper, "sta");
		break;
	case STX:
		instruction(system, cpu, write_mem, reg_x, i.a, 0, oper, "stx");
		break;
	case STY:
		instruction(system, cpu, write_mem, reg_y, i.a, 0, oper, "sty");
		break;
	case ADC:
		instruction(system, cpu, add, reg_a, i.a, 0, oper, "adc");
		break;
	case SBC:
		instruction(system, cpu, subtract, reg_a, i.a, 0, oper, "sbc");
		break;
	case INC:
		instruction(system, cpu, increment_mem, 0, i.a, 0, oper, "inc");
		break;
	case INX:
		instruction(system, cpu, increment_reg, reg_x, i.a, 0, oper, "inx");
		break;
	case INY:
		instruction(system, cpu, increment_reg, reg_y, i.a, 0, oper, "iny");
		break;
	case DEC:
		instruction(system, cpu, decrement_mem, 0, i.a, 0, oper, "dec");
		break;
	case DEX:
		instruction(system, cpu, decrement_reg, reg_x, i.a, 0, oper, "dex");
		break;
	case DEY:
		instruction(system, cpu, decrement_reg, reg_y, i.a, 0, oper, "dey");
		break;
	case ASL:
		instruction(system, cpu, arithmetic_shift_left, 0, i.a, 0, oper, "asl");
		break;
	case LSR:
		instruction(system, cpu, logical_shift_right, 0, i.a, 0, oper, "lsr");
		break;
	case ROL:
		instruction(system, cpu, shift_rol, 0, i.a, 0, oper, "rol");
		break;
	case ROR:
		instruction(system, cpu, shift_ror, 0, i.a, 0, oper, "ror");
		break;
	case AND:
		instruction(system, cpu, and_a, reg_a, i.a, 0, oper, "and");
		break;
	case ORA:
		instruction(system, cpu, or_a, reg_a, i.a, 0, oper, "ora");
		break;
	case EOR:
		instruction(system, cpu, xor_a, reg_a, i.a, 0, oper, "eor");
		break;
	case CMP:
		instruction(system, cpu, compare_reg_mem, reg_a, i.a, 0, oper, "cmp");
		break;
	case CPX:
		instruction(system, cpu, compare_reg_mem, reg_x, i.a, 0, oper, "cpx");
		break;
	case CPY:
		instruction(system, cpu, compare_reg_mem, reg_y, i.a, 0, oper, "cpy");
		break;
	case BIT:
		instruction(system, cpu, compare_bit, reg_a, i.a, 0, oper, "bit");
		break;
	case BCC:
		instruction(system, cpu, branch_conditional_flag_clear, 0, i.a, carry, oper, "bcc");
		break;
	case BCS:
		instruction(system, cpu, branch_conditional_flag, 0, i.a, carry, oper, "bcs");
		break;
	case BNE:
		instruction(system, cpu, branch_conditional_flag_clear, 0, i.a, zero, oper, "bne");
		break;
	case BEQ:
		instruction(system, cpu, branch_conditional_flag, 0, i.a, zero, oper, "beq");
		break;
	case BPL:
		instruction(system, cpu, branch_conditional_flag_clear, 0, i.a, negative, oper, "bpl");
		break;
	case BMI:
		instruction(system, cpu, branch_conditional_flag, 0, i.a, negative, oper, "bmi");
		break;
	case BVC:
		instruction(system, cpu, branch_conditional_flag_clear, 0, i.a, overflow, oper, "bvc");
		break;
	case BVS:
		instruction(system, cpu, branch_conditional_flag, 0, i.a, overflow, oper, "bvs");
		break;
	case TAX:
		instruction(system, cpu, transfer_reg_x, reg_a, i.a, 0, oper, "tax");
		break;
	case TXA:
		instruction(system, cpu, transfer_reg_a, reg_x, i.a, 0, oper, "txa");
		break;
	case TAY:
		instruction(system, cpu, transfer_reg_y, reg_a, i.a, 0, oper, "tay");
		break;
	case TYA:
		instruction(system, cpu, transfer_reg_a, reg_y, i.a, 0, oper, "tya");
		break;
	case TSX:
		instruction(system, cpu, transfer_reg_x, reg_sp, i.a, 0, oper, "tsx");
		break;
	case TXS:
		instruction(system, cpu, transfer_reg_sp, reg_x, i.a, 0, oper, "txs");
		break;
	case PHA:
		instruction(system, cpu, push_reg_stack, reg_a, i.a, 0, oper, "pha");
		break;
	case PLA:
		instruction(system, cpu, instruction_pla, reg_a, i.a, 0, oper, "pla");
		break;
	case PHP:
		instruction(system, cpu, instruction_php, reg_p, i.a, 0, oper, "php");
		break;
	case PLP:
		instruction(system, cpu, instruction_plp, reg_p, i.a, 0, oper, "plp");
		break;
	case JMP:
		instruction(system, cpu, branch, 0, i.a, 0, oper, "jmp");
		break;
	case JSR:
		instruction(system, cpu, branch_jsr, 0, i.a, 0, oper, "jsr");
		break;
	case RTS:
		instruction(system, cpu, branch_rts, 0, i.a, 0, oper, "rts");
		break;
	case RTI:
		instruction(system, cpu, branch_rti, 0, i.a, 0, oper, "rti");
		break;
	case CLC:
		instruction(system, cpu, clear_flag, 0, i.a, carry, oper, "clc");
		break;
	case SEC:
		instruction(system, cpu, set_flag, 0, i.a, carry, oper, "sec");
		break;
	case CLD:
		instruction(system, cpu, clear_flag, 0, i.a, decimal, oper, "cld");
		break;
	case SED:
		instruction(system, cpu, set_flag, 0, i.a, decimal, oper, "sed");
		break;
	case CLI:
		instruction(system, cpu, clear_flag, 0, i.a, interrupt_disable, oper, "cli");
		break;
	case SEI:
		instruction(system, cpu, set_flag, 0, i.a, interrupt_disable, oper, "sei");
		break;
	case CLV:
		instruction(system, cpu, clear_flag, 0, i.a, overflow, oper, "clv");
		break;
	case BRK:
		instruction(system, cpu, branch_brk, 0, i.a, 0, oper, "brk");
		break;
	case NOP:
		instruction(system, cpu, no_op, 0, i.a, 0, oper, "nop");
		break;
	default:
	case unimplemented:
		printf("Unimplemented opcode %X", i.n);
		break;
	}
	if (!cpu->branch_taken) {
		switch (i.a) {
		case accumulator:
		case implied:
			cpu->pc = cpu->pc + 1;
			break;
		case immediate:
		case zeropage:
		case zeropage_x:
		case zeropage_y:
		case zeropage_xi:
		case zeropage_yi:
		case relative:
			cpu->pc = cpu->pc + 2;
			break;
		case absolute:
		case absolute_indirect:
		case absolute_x:
		case absolute_y:
			cpu->pc = cpu->pc + 3;
			break;
		}
	}
}

Instruction parse(byte opcode)
{
	Instruction p;
	p.o = opcode;
	p.m = "xxx";
	switch (opcode)
	{
	case 0xEA: p.a = implied; p.n = NOP; p.m = "nop";
		break;

	case 0x69: p.a = immediate; p.n = ADC; p.m = "adc";
		break;
	case 0x65: p.a = zeropage; p.n = ADC; p.m = "adc";
		break;
	case 0x75: p.a = zeropage_x; p.n = ADC; p.m = "adc";
		break;
	case 0x6D: p.a = absolute; p.n = ADC; p.m = "adc";
		break;
	case 0x7D: p.a = absolute_x; p.n = ADC; p.m = "adc";
		break;
	case 0x79: p.a = absolute_y; p.n = ADC; p.m = "adc";
		break;
	case 0x61: p.a = zeropage_xi; p.n = ADC; p.m = "adc";
		break;
	case 0x71: p.a = zeropage_yi; p.n = ADC; p.m = "adc";
		break;

	case 0x29: p.a = immediate; p.n = AND; p.m = "and";
		break;
	case 0x25: p.a = zeropage; p.n = AND; p.m = "and";
		break;
	case 0x35: p.a = zeropage_x; p.n = AND; p.m = "and";
		break;
	case 0x2D: p.a = absolute; p.n = AND; p.m = "and";
		break;
	case 0x3D: p.a = absolute_x; p.n = AND; p.m = "and";
		break;
	case 0x39: p.a = absolute_y; p.n = AND; p.m = "and";
		break;
	case 0x21: p.a = zeropage_xi; p.n = AND; p.m = "and";
		break;
	case 0x31: p.a = zeropage_yi; p.n = AND; p.m = "and";
		break;

	case 0x0A: p.a = accumulator; p.n = ASL; p.m = "asl";
		break;
	case 0x06: p.a = zeropage; p.n = ASL; p.m = "asl";
		break;
	case 0x16: p.a = zeropage_x; p.n = ASL; p.m = "asl";
		break;
	case 0x0E: p.a = absolute; p.n = ASL; p.m = "asl";
		break;
	case 0x1E: p.a = absolute_x; p.n = ASL; p.m = "asl";
		break;

	case 0x90: p.a = relative; p.n = BCC; p.m = "bcc";
		break;
	case 0xB0: p.a = relative; p.n = BCS; p.m = "bcs";
		break;
	case 0xF0: p.a = relative; p.n = BEQ; p.m = "beq";
		break;

	case 0x24: p.a = zeropage; p.n = BIT; p.m = "bit";
		break;
	case 0x2C: p.a = absolute; p.n = BIT; p.m = "bit";
		break;

	case 0x30: p.a = relative; p.n = BMI; p.m = "bmi";
		break;
	case 0xD0: p.a = relative; p.n = BNE; p.m = "bne";
		break;
	case 0x10: p.a = relative; p.n = BPL; p.m = "bpl";
		break;
	case 0x00: p.a = implied; p.n = BRK; p.m = "brk";
		break;
	case 0x50: p.a = relative; p.n = BVC; p.m = "bvc";
		break;
	case 0x70: p.a = relative; p.n = BVS; p.m = "bvs";
		break;
	case 0x18: p.a = implied; p.n = CLC; p.m = "clc";
		break;
	case 0xD8: p.a = implied; p.n = CLD; p.m = "cld";
		break;
	case 0x58: p.a = implied; p.n = CLI; p.m = "cli";
		break;
	case 0xB8: p.a = implied; p.n = CLV; p.m = "clv";
		break;

	case 0xC9: p.a = immediate; p.n = CMP; p.m = "cmp";
		break;
	case 0xC5: p.a = zeropage; p.n = CMP; p.m = "cmp";
		break;
	case 0xD5: p.a = zeropage_x; p.n = CMP; p.m = "cmp";
		break;
	case 0xCD: p.a = absolute; p.n = CMP; p.m = "cmp";
		break;
	case 0xDD: p.a = absolute_x; p.n = CMP; p.m = "cmp";
		break;
	case 0xD9: p.a = absolute_y; p.n = CMP; p.m = "cmp";
		break;
	case 0xC1: p.a = zeropage_xi; p.n = CMP; p.m = "cmp";
		break;
	case 0xD1: p.a = zeropage_yi; p.n = CMP; p.m = "cmp";
		break;

	case 0xE0: p.a = immediate; p.n = CPX; p.m = "cpx";
		break;
	case 0xE4: p.a = zeropage; p.n = CPX; p.m = "cpx";
		break;
	case 0xEC: p.a = absolute; p.n = CPX; p.m = "cpx";
		break;

	case 0xC0: p.a = immediate; p.n = CPY; p.m = "cpy";
		break;
	case 0xC4: p.a = zeropage; p.n = CPY; p.m = "cpy";
		break;
	case 0xCC: p.a = absolute; p.n = CPY; p.m = "cpy";
		break;

	case 0xC6: p.a = zeropage; p.n = DEC; p.m = "dec";
		break;
	case 0xD6: p.a = zeropage_x; p.n = DEC; p.m = "dec";
		break;
	case 0xCE: p.a = absolute; p.n = DEC; p.m = "dec";
		break;
	case 0xDE: p.a = absolute_x; p.n = DEC; p.m = "dec";
		break;

	case 0xCA: p.a = implied; p.n = DEX; p.m = "dex";
		break;
	case 0x88: p.a = implied; p.n = DEY; p.m = "dey";
		break;

	case 0x49: p.a = immediate; p.n = EOR; p.m = "eor";
		break;
	case 0x45: p.a = zeropage; p.n = EOR; p.m = "eor";
		break;
	case 0x55: p.a = zeropage_x; p.n = EOR; p.m = "eor";
		break;
	case 0x4D: p.a = absolute; p.n = EOR; p.m = "eor";
		break;
	case 0x5D: p.a = absolute_x; p.n = EOR; p.m = "eor";
		break;
	case 0x59: p.a = absolute_y; p.n = EOR; p.m = "eor";
		break;
	case 0x41: p.a = zeropage_xi; p.n = EOR; p.m = "eor";
		break;
	case 0x51: p.a = zeropage_yi; p.n = EOR; p.m = "eor";
		break;

	case 0xE6: p.a = zeropage; p.n = INC; p.m = "inc";
		break;
	case 0xF6: p.a = zeropage_x; p.n = INC; p.m = "inc";
		break;
	case 0xEE: p.a = absolute; p.n = INC; p.m = "inc";
		break;
	case 0xFE: p.a = absolute_x; p.n = INC; p.m = "inc";
		break;

	case 0xE8: p.a = implied; p.n = INX; p.m = "inx";
		break;
	case 0xC8: p.a = implied; p.n = INY; p.m = "iny";
		break;

	case 0x4C: p.a = absolute; p.n = JMP; p.m = "jmp";
		break;
	case 0x6C: p.a = absolute_indirect; p.n = JMP; p.m = "jmp";
		break;

	case 0x20: p.a = absolute; p.n = JSR; p.m = "jsr";
		break;

	case 0xAD: p.a = absolute; p.n = LDA; p.m = "lda";
		break;
	case 0xBD: p.a = absolute_x; p.n = LDA; p.m = "lda";
		break;
	case 0xB9: p.a = absolute_y; p.n = LDA; p.m = "lda";
		break;
	case 0xA9: p.a = immediate; p.n = LDA; p.m = "lda";
		break;
	case 0xA5: p.a = zeropage; p.n = LDA; p.m = "lda";
		break;
	case 0xA1: p.a = zeropage_xi; p.n = LDA; p.m = "lda";
		break;
	case 0xB5: p.a = zeropage_x; p.n = LDA; p.m = "lda";
		break;
	case 0xB1: p.a = zeropage_yi; p.n = LDA; p.m = "lda";
		break;

	case 0xA2: p.a = immediate; p.n = LDX; p.m = "ldx";
		break;
	case 0xA6: p.a = zeropage; p.n = LDX; p.m = "ldx";
		break;
	case 0xB6: p.a = zeropage_y; p.n = LDX; p.m = "ldx";
		break;
	case 0xAE: p.a = absolute; p.n = LDX; p.m = "ldx";
		break;
	case 0xBE: p.a = absolute_y; p.n = LDX; p.m = "ldx";
		break;

	case 0xA0: p.a = immediate; p.n = LDY; p.m = "ldy";
		break;
	case 0xA4: p.a = zeropage; p.n = LDY; p.m = "ldy";
		break;
	case 0xB4: p.a = zeropage_x; p.n = LDY; p.m = "ldy";
		break;
	case 0xAC: p.a = absolute; p.n = LDY; p.m = "ldy";
		break;
	case 0xBC: p.a = absolute_x; p.n = LDY; p.m = "ldy";
		break;

	case 0x4A: p.a = accumulator; p.n = LSR; p.m = "lsr";
		break;
	case 0x46: p.a = zeropage; p.n = LSR; p.m = "lsr";
		break;
	case 0x56: p.a = zeropage_x; p.n = LSR; p.m = "lsr";
		break;
	case 0x4E: p.a = absolute; p.n = LSR; p.m = "lsr";
		break;
	case 0x5E: p.a = absolute_x; p.n = LSR; p.m = "lsr";
		break;

	case 0x09: p.a = immediate; p.n = ORA; p.m = "ora";
		break;
	case 0x05: p.a = zeropage; p.n = ORA; p.m = "ora";
		break;
	case 0x15: p.a = zeropage_x; p.n = ORA; p.m = "ora";
		break;
	case 0x0D: p.a = absolute; p.n = ORA; p.m = "ora";
		break;
	case 0x1D: p.a = absolute_x; p.n = ORA; p.m = "ora";
		break;
	case 0x19: p.a = absolute_y; p.n = ORA; p.m = "ora";
		break;
	case 0x01: p.a = zeropage_xi; p.n = ORA; p.m = "ora";
		break;
	case 0x11: p.a = zeropage_yi; p.n = ORA; p.m = "ora";
		break;

	case 0x48: p.a = implied; p.n = PHA; p.m = "pha";
		break;
	case 0x08: p.a = implied; p.n = PHP; p.m = "php";
		break;
	case 0x68: p.a = implied; p.n = PLA; p.m = "pla";
		break;
	case 0x28: p.a = implied; p.n = PLP; p.m = "plp";
		break;

	case 0x2A: p.a = accumulator; p.n = ROL; p.m = "rol";
		break;
	case 0x26: p.a = zeropage; p.n = ROL; p.m = "rol";
		break;
	case 0x36: p.a = zeropage_x; p.n = ROL; p.m = "rol";
		break;
	case 0x2E: p.a = absolute; p.n = ROL; p.m = "rol";
		break;
	case 0x3E: p.a = absolute_x; p.n = ROL; p.m = "rol";
		break;

	case 0x6A: p.a = accumulator; p.n = ROR; p.m = "ror";
		break;
	case 0x66: p.a = zeropage; p.n = ROR; p.m = "ror";
		break;
	case 0x76: p.a = zeropage_x; p.n = ROR; p.m = "ror";
		break;
	case 0x6E: p.a = absolute; p.n = ROR; p.m = "ror";
		break;
	case 0x7E: p.a = absolute_x; p.n = ROR; p.m = "ror";
		break;

	case 0x40: p.a = implied; p.n = RTI; p.m = "rti";
		break;
	case 0x60: p.a = implied; p.n = RTS; p.m = "rts";
		break;

	case 0xE9: p.a = immediate; p.n = SBC; p.m = "sbc";
		break;
	case 0xE5: p.a = zeropage; p.n = SBC; p.m = "sbc";
		break;
	case 0xF5: p.a = zeropage_x; p.n = SBC; p.m = "sbc";
		break;
	case 0xED: p.a = absolute; p.n = SBC; p.m = "sbc";
		break;
	case 0xFD: p.a = absolute_x; p.n = SBC; p.m = "sbc";
		break;
	case 0xF9: p.a = absolute_y; p.n = SBC; p.m = "sbc";
		break;
	case 0xE1: p.a = zeropage_xi; p.n = SBC; p.m = "sbc";
		break;
	case 0xF1: p.a = zeropage_yi; p.n = SBC; p.m = "sbc";
		break;

	case 0x38: p.a = implied; p.n = SEC; p.m = "sec";
		break;
	case 0xF8: p.a = implied; p.n = SED; p.m = "sed";
		break;
	case 0x78: p.a = implied; p.n = SEI; p.m = "sei";
		break;

	case 0x85: p.a = zeropage; p.n = STA; p.m = "sta";
		break;
	case 0x95: p.a = zeropage_x; p.n = STA; p.m = "sta";
		break;
	case 0x8D: p.a = absolute; p.n = STA; p.m = "sta";
		break;
	case 0x9D: p.a = absolute_x; p.n = STA; p.m = "sta";
		break;
	case 0x99: p.a = absolute_y; p.n = STA; p.m = "sta";
		break;
	case 0x81: p.a = zeropage_xi; p.n = STA; p.m = "sta";
		break;
	case 0x91: p.a = zeropage_yi; p.n = STA; p.m = "sta";
		break;

	case 0x86: p.a = zeropage; p.n = STX; p.m = "stx";
		break;
	case 0x96: p.a = zeropage_y; p.n = STX; p.m = "stx";
		break;
	case 0x8E: p.a = absolute; p.n = STX; p.m = "stx";
		break;

	case 0x84: p.a = zeropage; p.n = STY; p.m = "sty";
		break;
	case 0x94: p.a = zeropage_x; p.n = STY; p.m = "sty";
		break;
	case 0x8C: p.a = absolute; p.n = STY; p.m = "sty";
		break;

	case 0xAA: p.a = implied; p.n = TAX; p.m = "tax";
		break;
	case 0xA8: p.a = implied; p.n = TAY; p.m = "tay";
		break;
	case 0xBA: p.a = implied; p.n = TSX; p.m = "tsx";
		break;
	case 0x8A: p.a = implied; p.n = TXA; p.m = "txa";
		break;
	case 0x9A: p.a = implied; p.n = TXS; p.m = "txs";
		break;
	case 0x98: p.a = implied; p.n = TYA; p.m = "tya";
		break;
	default: p.a = implied; p.n = unimplemented; p.m = "XXX";
		break;
	}
	return p;
}

void write_cpu_state (Cpu_6502* cpu, System system, FILE* f)
{
	Instruction i = parse(MEM_READ(cpu->pc));

	byte oper1 = MEM_READ(cpu->pc + 1);
	byte oper2 = MEM_READ(cpu->pc + 2);

	word opera = bytes_to_word(oper2, oper1);
	char addr_mode[50];
	char addr_mode_n[50];
        switch (i.a) {
        case zeropage:
		snprintf(addr_mode_n, sizeof(addr_mode_n), "(zp)");
		break;
        case relative:
		snprintf(addr_mode_n, sizeof(addr_mode_n), "(rel)");
		break;
        case immediate:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(imm)");
	       break;
        case implied:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(imp)");
	       break;
        case accumulator:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(acc)");
	       break;
        case absolute:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(abs)");
	       break;
        case absolute_indirect:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(abs_i)");
	       break;
        case absolute_x:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(abs_x)");
	       break;
        case absolute_y:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(abs_y)");
	       break;
        case zeropage_x:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(zp_x)");
	       break;
        case zeropage_y:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(zp_y)");
	       break;
        case zeropage_xi:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(zp_xi)");
	       break;
        case zeropage_yi:
	       snprintf(addr_mode_n, sizeof(addr_mode_n), "(zp_yi)");
	       break;
        }
	switch (i.a) {
	case accumulator: case implied:
		snprintf(addr_mode, sizeof(addr_mode), " ");
		break;
	case immediate: case zeropage: case zeropage_x: case zeropage_y: case zeropage_xi: case zeropage_yi:
		snprintf(addr_mode, sizeof(addr_mode), "%X ", oper1);
		break;
	case relative:
		snprintf(addr_mode, sizeof(addr_mode), "0x%X", cpu->pc + (int8_t)oper1);
		break;
	case absolute: case absolute_indirect: case absolute_x: case absolute_y:
		snprintf(addr_mode, sizeof(addr_mode), "0x%X", opera);
		break;
	}
	byte oper[] = {oper1, oper2};
	fprintf(f, "%X %s %s %s  -  ", i.o, addr_mode_n, i.m, addr_mode);

	char* reg_names[] = {
		"A",
		"X",
		"Y",
		"SP",
		"P",
	};
	for(int i=0;i<sizeof(cpu->reg)/sizeof(byte);i++) {
		fprintf(f, "%s:", reg_names[i]);
		fprintf(f, "$%X ", cpu->reg[i]);
	}
	fprintf(f, "PC:$%04X ", cpu->pc);
	fprintf(f,"NVUBDIZC ");
	for (int i = 7; 0 <= i; i--) {
		fprintf(f, "%c", (cpu->reg[reg_p] & (1 << i)) ? '1' : '0');
	}
	fprintf(f, "\n");
}

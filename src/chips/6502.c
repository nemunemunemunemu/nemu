#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include "../types.h"
#include "../bitmath.h"
#include "../systems/system.h"
#include "2C02.h"
#include "6502.h"
#include "../systems/famicom.h"

void set_p ( Cpu_6502* cpu, enum flag f, bool value )
{
	cpu->reg[reg_p] = set_bit(cpu->reg[reg_p], f, value);
}

byte get_p ( Cpu_6502* cpu, enum flag f )
{
	return get_bit(cpu->reg[reg_p], f);
}

void set_reg (Cpu_6502* cpu, enum register_ r, byte value)
{
	cpu->reg[r] = value;
	return;
}

byte mmap_6502 (System system, word addr, byte value, bool write)
{
	switch (system.s) {
	case famicom_system:
		return mmap_famicom(system.h, addr, value, write);
	default:
		return 0;
	}
}

void mem_write(System system, word addr, byte value )
{
	mmap_6502(system, addr, value, true);
}

byte mem_read(System system, word addr )
{
	return mmap_6502(system, addr, 0, false);
}

void cpu_reset(Cpu_6502* cpu, System system)
{
	memset( cpu->reg, 0, sizeof(cpu->reg) * sizeof(byte) );
	cpu->reg[reg_p] = 0x20; // 00100000 (the unused flag needs to be set)
	cpu->reg[reg_sp] = 0xFD;
	cpu->pc = bytes_to_word(mem_read(system, 0xFFFD), mem_read(system, 0xFFFC));
	cpu->running = true;
	cpu->current_instruction_name = "";
}

void push_stack(System system, Cpu_6502* cpu, byte value)
{
	mem_write(system, 0x100 + cpu->reg[reg_sp], value);
	set_reg(cpu, reg_sp, cpu->reg[reg_sp] - 1);
}

byte pull_stack(System system, Cpu_6502* cpu)
{
	set_reg(cpu, reg_sp, cpu->reg[reg_sp] + 1);
	return mem_read(system, 0x100 + cpu->reg[reg_sp]);
}

byte address (System system, Cpu_6502* cpu, byte oper[2], enum addressing_mode a, bool write, byte value)
{
	word addr_a = bytes_to_word(oper[1], oper[0]);
	word addr_x;
	word addr_y;
	word addr_f;
	switch (a) {
	case immediate:
		return oper[0];
        case relative:
		if (!write)
			return mem_read(system, cpu->pc + (int8_t)oper[0] );
		return 0;
		break;
	case accumulator:
		if (write) {
			set_reg(cpu, reg_a, value);
			return 0;
		} else {
			return cpu->reg[reg_a];
		}
		break;
	case zeropage:
	case absolute:
		if (write) {
			mem_write(system, addr_a, value);
			return 0;
		} else {
			return mem_read(system, addr_a);
		}
		break;
	case absolute_indirect:
		addr_f = bytes_to_word(mem_read(system,addr_a+1), mem_read(system,addr_a));
		if (write) {
			mem_write(system, addr_f, value);
			return 0;
		} else {
			return mem_read(system, addr_f);
		}
		break;
	case zeropage_x:
		addr_f = (oper[0] + cpu->reg[reg_x]) % 0x100;
		if (write) {
			mem_write(system, addr_f, value);
			return 0;
		} else {
			return mem_read(system, addr_f);
		}
		break;
	case zeropage_y:
		addr_f = (oper[0] + cpu->reg[reg_y]) % 0x100;
		if (write) {
			mem_write(system, addr_f, value);
			return 0;
		} else {
			return mem_read(system, addr_f);
		}
		break;
	case absolute_x:
		addr_f = addr_a + cpu->reg[reg_x];
		if (write) {
			mem_write(system, addr_f, value);
			return 0;
		} else {
			return mem_read(system, addr_f);
		}
		break;
	case absolute_y:
		addr_f = addr_a + cpu->reg[reg_y];
		if (write) {
			mem_write(system, addr_f, value);
			return 0;
		} else {
			return mem_read(system, addr_f);
		}
		break;
	case zeropage_xi:
		addr_x = (oper[0] + cpu->reg[reg_x]) % 0x100;
		addr_f = bytes_to_word(mem_read(system, addr_x+1), mem_read(system, addr_x));
		if (write) {
			mem_write(system, addr_f, value);
			return 0;
		} else {
			return mem_read(system, addr_f);
		}
		break;
	case zeropage_yi:
		addr_f = bytes_to_word(mem_read(system, oper[0]+1), mem_read(system, oper[0])) + cpu->reg[reg_y];
		if (write) {
			mem_write(system, addr_f, value);
			return 0;
		} else {
			return mem_read(system, addr_f);
		}
		break;
	default:
		return 0;
		break;
	}
}

void update_p_nz ( Cpu_6502* cpu, byte value )
{
	set_p(cpu, negative, get_bit(value, 0) == 1);
	if (value == 0) {
		set_p(cpu, zero, true);
	} else {
		set_p(cpu, zero, false);
	}
}

void update_p_c ( Cpu_6502* cpu, byte value_old, byte value)
{
	if (value < value_old) {
		set_p(cpu, carry, true);
	} else {
		set_p(cpu, carry, false);
	}
	if (get_bit(value, 0) != get_p(cpu, carry)) {
		set_p(cpu, overflow, true);
	} else {
		set_p(cpu, overflow, false);
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
		update_p_nz(cpu, value);
		break;

	case increment_mem:
		value = peek(system, cpu, a, oper);
		poke(system, cpu, a, oper, value + 1);
		update_p_nz(cpu, value);
		break;

	case decrement_mem:
		value = peek(system, cpu, a, oper);
		poke(system, cpu, a, oper, value - 1);
		update_p_nz(cpu, value);
		break;

	case shift_rol:
		value = peek(system, cpu, a, oper);
		set_p(cpu, carry, get_bit(value, 7));
		value = value << 1;
		value = set_bit(value, 0, get_p(cpu, carry));
		poke(system, cpu, a, oper, value);
		update_p_nz(cpu, value);
		break;

	case shift_ror:
		value = peek(system, cpu, a, oper);
		set_p(cpu, carry, get_bit(value, 0));
		value = value >> 1;
		value = set_bit(value, 7, get_p(cpu, carry));
		poke(system, cpu, a, oper, value);
		update_p_nz(cpu, value);
                break;

	case logical_shift_right:
		value = peek(system, cpu, a, oper);
		set_p(cpu, carry, get_bit(value, 7));
		value >>= 1;
		poke(system, cpu, a, oper, value);
		set_p(cpu, zero, cpu->reg[reg_a] == 0);
		set_p(cpu, negative, get_bit(value, 0));
		break;

	case arithmetic_shift_left:
		value = peek(system, cpu, a, oper);
		set_p(cpu, carry, get_bit(value, 0));
		value <<= 1;
		poke(system, cpu, a, oper, value);
		update_p_nz(cpu, value);
		break;

	case compare_reg_mem:
		value = peek(system, cpu, a, oper);
		if (get_bit(value - cpu->reg[r], 0) == 1) {
			set_p(cpu, negative, true);
		} else {
			set_p(cpu, negative, false);
		}
		set_p(cpu, zero, cpu->reg[r] == value);
		set_p(cpu, carry, cpu->reg[r] >= value);
		break;

	case compare_mem_accumulator:
		value = peek(system, cpu, a, oper);
		if ((cpu->reg[reg_a] & value) == 0) {
			set_p(cpu, zero, true);
		} else {
			set_p(cpu, zero, false);
		}
		set_p(cpu, negative, get_bit(value, 0));
		set_p(cpu, overflow, get_bit(value, 1));
		break;

		//register
	case alter_register:
		value = peek(system, cpu, a, oper);
		set_reg( cpu, r,  value);
		update_p_nz(cpu, value);
		break;

	case add:
		value_old = peek(system, cpu, a, oper);
		value = value_old + cpu->reg[reg_a] + get_p(cpu, carry);
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		update_p_c(cpu, value_old, value);
		break;

	case subtract:
		value_old = cpu->reg[reg_a];
		value = (cpu->reg[reg_a] - peek(system, cpu, a, oper)) - !get_p(cpu, carry);
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		update_p_c(cpu, value_old, value);
		break;

	case increment_reg:
		value_old = cpu->reg[r];
		value = cpu->reg[r] + 1;
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		break;

	case decrement_reg:
		value_old = cpu->reg[r];
		value = cpu->reg[r] - 1;
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		break;

	case and_a:
		value = peek(system, cpu, a, oper);
		set_reg(cpu, reg_a, value & cpu->reg[reg_a]);
		update_p_nz(cpu, value);
		break;

	case or_a:
		value = peek(system, cpu, a, oper);
		set_reg(cpu, reg_a, value | cpu->reg[reg_a]);
		update_p_nz(cpu, value);
		break;

	case xor_a:
		value = peek(system, cpu, a, oper);
		set_reg(cpu, reg_a, value ^ cpu->reg[reg_a]);
		update_p_nz(cpu, value);
		break;

	case transfer_reg_a:
		value = cpu->reg[reg_a];
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		break;
	case transfer_reg_x:
		value = cpu->reg[reg_x];
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		break;
	case transfer_reg_sp:
		value = cpu->reg[reg_sp];
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		break;
	case transfer_reg_y:
		value = cpu->reg[reg_y];
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		break;

		//branch
        case branch:
		if (a == absolute) {
			addr = opera;
		} else {
			addr = peek(system, cpu, a, oper);
		}
		cpu->pc = addr;
		cpu->branch_taken = true;
		break;

	case branch_jsr:
		push_stack(system, cpu, get_higher_byte(cpu->pc + 3));
		push_stack(system, cpu, get_lower_byte(cpu->pc + 3));
                cpu->pc = opera;
		cpu->branch_taken = true;
		break;

	case branch_rts:
		high = pull_stack(system, cpu);
		low = pull_stack(system, cpu);
		addr = bytes_to_word(low, high);
		cpu->pc = addr;
		cpu->branch_taken = true;
		break;

	// not setting cpu->branch_taken on these is intentional behavior
	case branch_rti:
		set_reg(cpu, reg_p, pull_stack(system, cpu));
		high = pull_stack(system, cpu);
		low = pull_stack(system, cpu);
		addr = bytes_to_word(low, high);
		cpu->pc = addr;
		break;

	case branch_conditional_flag:
		addr = (int8_t)oper[0] + cpu->pc;
		if (get_p(cpu, f) == 1) {
			cpu->pc = addr;
		}
		break;

	case branch_conditional_flag_clear:
		addr = (int8_t)oper[0] + cpu->pc;
		if (get_p(cpu, f) == 0) {
			cpu->pc = addr;
		}
                break;

	case push_reg_stack:
		push_stack(system, cpu, cpu->reg[r]);
		break;
	case pull_reg_stack:
		value = pull_stack(system, cpu);
		set_reg(cpu, r, value);
		update_p_nz(cpu, value);
		break;
	case set_flag:
		set_p(cpu, f, true);
		break;
	case clear_flag:
		set_p(cpu, f, false);
		break;
	case break_op:
		set_p(cpu, break_, true);
		set_p(cpu, interrupt_disable, true);
		break;
        }
}


void nmi(System system, Cpu_6502* cpu)
{
	push_stack(system, cpu, get_higher_byte(cpu->pc));
	push_stack(system, cpu, get_lower_byte(cpu->pc));
	set_p(cpu, break_, 0);
	push_stack(system, cpu, cpu->reg[reg_p]);
	cpu->pc = bytes_to_word(mem_read(system, 0xFFFB), mem_read(system, 0xFFFA));
}

void print_cpu_state (Cpu_6502* cpu)
{
	char* reg_names[] = {
		"A",
		"X",
		"Y",
		"SP",
		"P",
	};
	for(int i=0;i<sizeof(cpu->reg)/sizeof(byte);i++) {
		printf("%s:", reg_names[i]);
		printf("$%X ", cpu->reg[i]);
	}
	printf("PC:$%04X \n", cpu->pc);
	printf("NVUBDIZC ");
	print_byte_as_bits(cpu->reg[reg_p]);
}

void print_addressing_mode(enum addressing_mode a)
{
       switch (a) {
       case zeropage:
	       printf("(zp)");
	       break;
       case relative:
	       printf("(rel)");
	       break;
       case immediate:
	       printf("(imm)");
	       break;
       case implied:
	       printf("(imp)");
	       break;
       case accumulator:
	       printf("(acc)");
	       break;
       case absolute:
	       printf("(abs)");
	       break;
       case absolute_indirect:
	       printf("(abs_i)");
	       break;
       case absolute_x:
	       printf("(abs_x)");
	       break;
       case absolute_y:
	       printf("(abs_y)");
	       break;
       case zeropage_x:
	       printf("(zp_x)");
	       break;
       case zeropage_y:
	       printf("(zp_y)");
	       break;
       case zeropage_xi:
	       printf("(zp_xi)");
	       break;
       case zeropage_yi:
	       printf("(zp_yi) ");
	       break;
       }
}

void nop ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, no_op, 0, a, 0, oper, "nop");
}

void brk_ ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, no_op, 0, a, 0, oper, "brk");
}

// store

void lda ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, alter_register, reg_a, a, 0, oper, "lda");
}

void ldx ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, alter_register, reg_x, a, 0, oper, "ldx");
}

void ldy ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, alter_register, reg_y, a, 0, oper, "ldy");
}

void sta ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, write_mem, reg_a, a, 0, oper, "sta");
}

void stx ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, write_mem, reg_x, a, 0, oper, "stx");
}

void sty ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, write_mem, reg_y, a, 0, oper, "sty");
}

// arithmetic

void adc ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, add, reg_a, a, 0, oper, "adc");
}

void sbc ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, subtract, reg_a, a, 0, oper, "adc");
}

// increment / decrement

void inc ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, increment_mem, 0, a, 0, oper, "inc");
}

void dec ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, decrement_mem, 0, a, 0, oper, "dec");
}

void inx ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, increment_reg, reg_x, a, 0, oper, "inx");
}

void dex ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, decrement_reg, reg_x, a, 0, oper, "dex");
}

void iny ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, increment_reg, reg_y, a, 0, oper, "iny");
}

void dey ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, decrement_reg, reg_y, a, 0, oper, "dey");
}

void and ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, and_a, reg_a, a, 0, oper, "and");
}

void ora ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, or_a, reg_a, a, 0, oper, "ora");
}

void eor ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, or_a, reg_a, a, 0, oper, "eor");
}

void cmp ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, compare_reg_mem, reg_a, a, 0, oper, "cmp");
}

void cpx ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, compare_reg_mem, reg_x, a, 0, oper, "cpx");
}

void cpy ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, compare_reg_mem, reg_y, a, 0, oper, "cpy");
}

void bit ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, compare_mem_accumulator, reg_a, a, 0, oper, "bit");
}

void asl ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, arithmetic_shift_left, 0, a, 0, oper, "asl");
}

void lsr ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, logical_shift_right, 0, a, 0, oper, "lsr");
}

void rol ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, shift_rol, 0, a, 0, oper, "rol");
}

void ror ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, shift_ror, 0, a, 0, oper, "ror");
}


//branch
void jmp ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch, 0, a, 0, oper, "jmp");
}

void jsr ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_jsr, 0, a, 0, oper, "jsr");
}

void rts ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_rts, 0, a, 0, oper, "rts");
}

void rti ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_rti, 0, a, 0, oper, "rti");
}

void bcc ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag_clear, 0, a, carry, oper, "bcc");
}

void bcs ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag, 0, a, carry, oper, "bcs");
}

void bne ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag_clear, 0, a, zero, oper, "bne");
}

void beq ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag, 0, a, zero, oper, "beq");
}

void bpl ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag_clear, 0, a, negative, oper, "bpl");
}

void bmi ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag, 0, a, zero, oper, "bmi");
}

void bvc ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag_clear, 0, a, overflow, oper, "bvc");
}

void bvs ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, branch_conditional_flag, 0, a, overflow, oper, "bvs");
}

// flags

void clc ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, clear_flag, 0, a, carry, oper, "clc");
}

void sec ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, set_flag, 0, a, carry, oper, "sec");
}

void cld ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, clear_flag, 0, a, decimal, oper, "cld");
}

void sed ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, set_flag, 0, a, decimal, oper, "sed");
}

void cli ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, clear_flag, 0, a, interrupt_disable, oper, "cli");
}

void sei ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, set_flag, 0, a, decimal, oper, "sei");
}

void clv ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, clear_flag, 0, a, overflow, oper, "clv");
}


// transfer
void tax ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, transfer_reg_a, reg_x, a, 0, oper, "tax");
}

void txa ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, transfer_reg_x, reg_a, a, 0, oper, "txa");
}

void tay ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, transfer_reg_a, reg_y, a, 0, oper, "tay");
}

void tya ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, transfer_reg_y, reg_a, a, 0, oper, "tya");
}

void tsx ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, transfer_reg_sp, reg_x, a, 0, oper, "tsx");
}

void txs ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, transfer_reg_x, reg_sp, a, 0, oper, "txs");
}

// stack

void pha ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, push_reg_stack, reg_a, a, 0, oper, "pha");
}

void pla ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, pull_reg_stack, reg_a, a, 0, oper, "pla");
}

void php ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, push_reg_stack, reg_p, a, 0, oper, "php");
}

void plp ( System system, Cpu_6502* cpu, enum addressing_mode a, byte oper[2] )
{
	instruction(system, cpu, pull_reg_stack, reg_p, a, 0, oper, "plp");
}

void step(System system, Cpu_6502* cpu, Instruction i, byte oper[2])
{
	cpu->branch_taken = false;
	switch (i.n)
	{
	case LDA:
		lda(system, cpu, i.a, oper);
		break;
	case LDX:
		ldx(system, cpu, i.a, oper);
		break;
	case LDY:
		ldy(system, cpu, i.a, oper);
		break;
	case STA:
		sta(system, cpu, i.a, oper);
		break;
	case STX:
		stx(system, cpu, i.a, oper);
		break;
	case STY:
		sty(system, cpu, i.a, oper);
		break;
	case ADC:
		adc(system, cpu, i.a, oper);
		break;
	case SBC:
		sbc(system, cpu, i.a, oper);
		break;
	case INC:
		inc(system, cpu, i.a, oper);
		break;
	case INX:
		inx(system, cpu, i.a, oper);
		break;
	case INY:
		iny(system, cpu, i.a, oper);
		break;
	case DEC:
		dec(system, cpu, i.a, oper);
		break;
	case DEX:
		dex(system, cpu, i.a, oper);
		break;
	case DEY:
		dey(system, cpu, i.a, oper);
		break;
	case ASL:
		asl(system, cpu, i.a, oper);
		break;
	case LSR:
		lsr(system, cpu, i.a, oper);
		break;
	case ROL:
		rol(system, cpu, i.a, oper);
		break;
	case ROR:
		ror(system, cpu, i.a, oper);
		break;
	case AND:
		and(system, cpu, i.a, oper);
		break;
	case ORA:
		ora(system, cpu, i.a, oper);
		break;
	case EOR:
		eor(system, cpu, i.a, oper);
		break;
	case CMP:
		cmp(system, cpu, i.a, oper);
		break;
	case CPX:
		cpx(system, cpu, i.a, oper);
		break;
	case CPY:
		cpy(system, cpu, i.a, oper);
		break;
	case BIT:
		bit(system, cpu, i.a, oper);
		break;
	case BCC:
		bcc(system, cpu, i.a, oper);
		break;
	case BCS:
		bcs(system, cpu, i.a, oper);
		break;
	case BNE:
		bne(system, cpu, i.a, oper);
		break;
	case BEQ:
		beq(system, cpu, i.a, oper);
		break;
	case BPL:
		bpl(system, cpu, i.a, oper);
		break;
	case BMI:
		bmi(system, cpu, i.a, oper);
		break;
	case BVC:
		bvc(system, cpu, i.a, oper);
		break;
	case BVS:
		bvs(system, cpu, i.a, oper);
		break;
	case TAX:
		tax(system, cpu, i.a, oper);
		break;
	case TXA:
		txa(system, cpu, i.a, oper);
		break;
	case TAY:
		tay(system, cpu, i.a, oper);
		break;
	case TYA:
		tya(system, cpu, i.a, oper);
		break;
	case TSX:
		tsx(system, cpu, i.a, oper);
		break;
	case TXS:
		txs(system, cpu, i.a, oper);
		break;
	case PHA:
		pha(system, cpu, i.a, oper);
		break;
	case PLA:
		pla(system, cpu, i.a, oper);
		break;
	case PHP:
		php(system, cpu, i.a, oper);
		break;
	case PLP:
		plp(system, cpu, i.a, oper);
		break;
	case JMP:
		jmp(system, cpu, i.a, oper);
		break;
	case JSR:
		jsr(system, cpu, i.a, oper);
		break;
	case RTS:
		rts(system, cpu, i.a, oper);
		break;
	case RTI:
		rti(system, cpu, i.a, oper);
		break;
	case CLC:
		clc(system, cpu, i.a, oper);
		break;
	case SEC:
		sec(system, cpu, i.a, oper);
		break;
	case CLD:
		cld(system, cpu, i.a, oper);
		break;
	case SED:
		sed(system, cpu, i.a, oper);
		break;
	case CLI:
		cli(system, cpu, i.a, oper);
		break;
	case SEI:
		sei(system, cpu, i.a, oper);
		break;
	case CLV:
		clv(system, cpu, i.a, oper);
		break;
	case BRK:
		brk_(system, cpu, i.a, oper);
		break;
	case NOP:
		nop(system, cpu, i.a, oper);
		break;
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
	switch (opcode)
	{
	case 0xEA: p.a = implied; p.n = NOP;
		break;

	case 0x69: p.a = immediate; p.n = ADC;
		break;
	case 0x65: p.a = zeropage; p.n = ADC;
		break;
	case 0x75: p.a = zeropage_x; p.n = ADC;
		break;
	case 0x6D: p.a = absolute; p.n = ADC;
		break;
	case 0x7D: p.a = absolute_x; p.n = ADC;
		break;
	case 0x79: p.a = absolute_y; p.n = ADC;
		break;
	case 0x61: p.a = zeropage_xi; p.n = ADC;
		break;
	case 0x71: p.a = zeropage_yi; p.n = ADC;
		break;

	case 0x29: p.a = immediate; p.n = AND;
		break;
	case 0x25: p.a = zeropage; p.n = AND;
		break;
	case 0x35: p.a = zeropage_x; p.n = AND;
		break;
	case 0x2D: p.a = absolute; p.n = AND;
		break;
	case 0x3D: p.a = absolute_x; p.n = AND;
		break;
	case 0x39: p.a = absolute_y; p.n = AND;
		break;
	case 0x21: p.a = zeropage_xi; p.n = AND;
		break;
	case 0x31: p.a = zeropage_yi; p.n = AND;
		break;

	case 0x0A: p.a = accumulator; p.n = ASL;
		break;
	case 0x06: p.a = zeropage; p.n = ASL;
		break;
	case 0x16: p.a = zeropage_x; p.n = ASL;
		break;
	case 0x0E: p.a = absolute; p.n = ASL;
		break;
	case 0x1E: p.a = absolute_x; p.n = ASL;
		break;

	case 0x90: p.a = relative; p.n = BCC;
		break;
	case 0xB0: p.a = relative; p.n = BCS;
		break;
	case 0xF0: p.a = relative; p.n = BEQ;
		break;

	case 0x24: p.a = zeropage; p.n = BIT;
		break;
	case 0x2C: p.a = absolute; p.n = BIT;
		break;

	case 0x30: p.a = relative; p.n = BMI;
		break;
	case 0xD0: p.a = relative; p.n = BNE;
		break;
	case 0x10: p.a = relative; p.n = BPL;
		break;
	case 0x00: p.a = implied; p.n = BRK;
		break;
	case 0x50: p.a = relative; p.n = BVC;
		break;
	case 0x70: p.a = relative; p.n = BVS;
		break;
	case 0x18: p.a = implied; p.n = CLC;
		break;
	case 0xD8: p.a = implied; p.n = CLD;
		break;
	case 0x58: p.a = implied; p.n = CLI;
		break;
	case 0xB8: p.a = implied; p.n = CLV;
		break;

	case 0xC9: p.a = immediate; p.n = CMP;
		break;
	case 0xC5: p.a = zeropage; p.n = CMP;
		break;
	case 0xD5: p.a = zeropage_x; p.n = CMP;
		break;
	case 0xCD: p.a = absolute; p.n = CMP;
		break;
	case 0xDD: p.a = absolute_x; p.n = CMP;
		break;
	case 0xD9: p.a = absolute_y; p.n = CMP;
		break;
	case 0xC1: p.a = zeropage_xi; p.n = CMP;
		break;
	case 0xD1: p.a = zeropage_yi; p.n = CMP;
		break;

	case 0xE0: p.a = immediate; p.n = CPX;
		break;
	case 0xE4: p.a = zeropage; p.n = CPX;
		break;
	case 0xEC: p.a = absolute; p.n = CPX;
		break;

	case 0xC0: p.a = immediate; p.n = CPY;
		break;
	case 0xC4: p.a = zeropage; p.n = CPY;
		break;
	case 0xCC: p.a = absolute; p.n = CPY;
		break;

	case 0xC6: p.a = zeropage; p.n = DEC;
		break;
	case 0xD6: p.a = zeropage_x; p.n = DEC;
		break;
	case 0xCE: p.a = absolute; p.n = DEC;
		break;
	case 0xDE: p.a = absolute_x; p.n = DEC;
		break;

	case 0xCA: p.a = implied; p.n = DEX;
		break;
	case 0x88: p.a = implied; p.n = DEY;
		break;

	case 0x49: p.a = immediate; p.n = EOR;
		break;
	case 0x45: p.a = zeropage; p.n = EOR;
		break;
	case 0x55: p.a = zeropage_x; p.n = EOR;
		break;
	case 0x4D: p.a = absolute; p.n = EOR;
		break;
	case 0x5D: p.a = absolute_x; p.n = EOR;
		break;
	case 0x59: p.a = absolute_y; p.n = EOR;
		break;
	case 0x41: p.a = zeropage_xi; p.n = EOR;
		break;
	case 0x51: p.a = zeropage_yi; p.n = EOR;
		break;

	case 0xE6: p.a = zeropage; p.n = INC;
		break;
	case 0xF6: p.a = zeropage_x; p.n = INC;
		break;
	case 0xEE: p.a = absolute; p.n = INC;
		break;
	case 0xFE: p.a = absolute_x; p.n = INC;
		break;

	case 0xE8: p.a = implied; p.n = INX;
		break;
	case 0xC8: p.a = implied; p.n = INY;
		break;

	case 0x4C: p.a = absolute; p.n = JMP;
		break;
	case 0x6C: p.a = absolute_indirect; p.n = JMP;
		break;

	case 0x20: p.a = absolute; p.n = JSR;
		break;

	case 0xAD: p.a = absolute; p.n = LDA;
		break;
	case 0xBD: p.a = absolute_x; p.n = LDA;
		break;
	case 0xB9: p.a = absolute_y; p.n = LDA;
		break;
	case 0xA9: p.a = immediate; p.n = LDA;
		break;
	case 0xA5: p.a = zeropage; p.n = LDA;
		break;
	case 0xA1: p.a = zeropage_xi; p.n = LDA;
		break;
	case 0xB5: p.a = zeropage_x; p.n = LDA;
		break;
	case 0xB1: p.a = zeropage_yi; p.n = LDA;
		break;

	case 0xA2: p.a = immediate; p.n = LDX;
		break;
	case 0xA6: p.a = zeropage; p.n = LDX;
		break;
	case 0xB6: p.a = zeropage_y; p.n = LDX;
		break;
	case 0xAE: p.a = absolute; p.n = LDX;
		break;
	case 0xBE: p.a = absolute_y; p.n = LDX;
		break;

	case 0xA0: p.a = immediate; p.n = LDY;
		break;
	case 0xA4: p.a = zeropage; p.n = LDY;
		break;
	case 0xB4: p.a = zeropage_x; p.n = LDY;
		break;
	case 0xAC: p.a = absolute; p.n = LDY;
		break;
	case 0xBC: p.a = absolute_x; p.n = LDY;
		break;

	case 0x4A: p.a = accumulator; p.n = LSR;
		break;
	case 0x46: p.a = zeropage; p.n = LSR;
		break;
	case 0x56: p.a = zeropage_x; p.n = LSR;
		break;
	case 0x4E: p.a = absolute; p.n = LSR;
		break;
	case 0x5E: p.a = absolute_x; p.n = LSR;
		break;

	case 0x09: p.a = immediate; p.n = ORA;
		break;
	case 0x05: p.a = zeropage; p.n = ORA;
		break;
	case 0x15: p.a = zeropage_x; p.n = ORA;
		break;
	case 0x0D: p.a = absolute; p.n = ORA;
		break;
	case 0x1D: p.a = absolute_x; p.n = ORA;
		break;
	case 0x19: p.a = absolute_y; p.n = ORA;
		break;
	case 0x01: p.a = zeropage_xi; p.n = ORA;
		break;
	case 0x11: p.a = zeropage_yi; p.n = ORA;
		break;

	case 0x48: p.a = implied; p.n = PHA;
		break;
	case 0x08: p.a = implied; p.n = PHP;
		break;
	case 0x68: p.a = implied; p.n = PLA;
		break;
	case 0x28: p.a = implied; p.n = PLP;
		break;

	case 0x2A: p.a = accumulator; p.n = ROL;
		break;
	case 0x26: p.a = zeropage; p.n = ROL;
		break;
	case 0x36: p.a = zeropage_x; p.n = ROL;
		break;
	case 0x2E: p.a = absolute; p.n = ROL;
		break;
	case 0x3E: p.a = absolute_x; p.n = ROL;
		break;

	case 0x6A: p.a = accumulator; p.n = ROR;
		break;
	case 0x66: p.a = zeropage; p.n = ROR;
		break;
	case 0x76: p.a = zeropage_x; p.n = ROR;
		break;
	case 0x6E: p.a = absolute; p.n = ROR;
		break;
	case 0x7E: p.a = absolute_x; p.n = ROR;
		break;

	case 0x40: p.a = implied; p.n = RTI;
		break;
	case 0x60: p.a = implied; p.n = RTS;
		break;

	case 0xE9: p.a = immediate; p.n = SBC;
		break;
	case 0xE5: p.a = zeropage; p.n = SBC;
		break;
	case 0xF5: p.a = zeropage_x; p.n = SBC;
		break;
	case 0xED: p.a = absolute; p.n = SBC;
		break;
	case 0xFD: p.a = absolute_x; p.n = SBC;
		break;
	case 0xF9: p.a = absolute_y; p.n = SBC;
		break;
	case 0xE1: p.a = zeropage_xi; p.n = SBC;
		break;
	case 0xF1: p.a = zeropage_yi; p.n = SBC;
		break;

	case 0x38: p.a = implied; p.n = SEC;
		break;
	case 0xF8: p.a = implied; p.n = SED;
		break;
	case 0x78: p.a = implied; p.n = SEI;
		break;

	case 0x85: p.a = zeropage; p.n = STA;
		break;
	case 0x95: p.a = zeropage_x; p.n = STA;
		break;
	case 0x8D: p.a = absolute; p.n = STA;
		break;
	case 0x9D: p.a = absolute_x; p.n = STA;
		break;
	case 0x99: p.a = absolute_y; p.n = STA;
		break;
	case 0x81: p.a = zeropage_xi; p.n = STA;
		break;
	case 0x91: p.a = zeropage_yi; p.n = STA;
		break;

	case 0x86: p.a = zeropage; p.n = STX;
		break;
	case 0x96: p.a = zeropage_y; p.n = STX;
		break;
	case 0x8E: p.a = absolute; p.n = STX;
		break;

	case 0x84: p.a = zeropage; p.n = STY;
		break;
	case 0x94: p.a = zeropage_x; p.n = STY;
		break;
	case 0x8C: p.a = absolute; p.n = STY;
		break;

	case 0xAA: p.a = implied; p.n = TAX;
		break;
	case 0xA8: p.a = implied; p.n = TAY;
		break;
	case 0xBA: p.a = implied; p.n = TSX;
		break;
	case 0x8A: p.a = implied; p.n = TXA;
		break;
	case 0x9A: p.a = implied; p.n = TXS;
		break;
	case 0x98: p.a = implied; p.n = TYA;
		break;
	default: p.a = implied; p.n = unimplemented;
	}
	return p;
}

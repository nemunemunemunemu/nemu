enum register_ {
	reg_a,
	reg_x,
	reg_y,
	reg_sp,
	reg_p,
};

enum operation {
	no_op,
	alter_register,
	write_mem,
	add,
	subtract,
	increment_mem,
	decrement_mem,
	increment_reg,
	decrement_reg,
	and_a,
	or_a,
	xor_a,
	shift_rol,
	shift_ror,
	logical_shift_right,
	arithmetic_shift_left,
	branch,
	branch_jsr,
	branch_rts,
	branch_rti,
	branch_conditional_flag,
	branch_conditional_flag_clear,
	compare_reg_mem,
	compare_bit,
	transfer_reg_a,
	transfer_reg_x,
	transfer_reg_sp,
	transfer_reg_y,
	push_reg_stack,
	pull_reg_stack,
	set_flag,
	clear_flag,
	break_op
};

enum flag {
	carry,
	zero,
	interrupt_disable,
	decimal,
	break_,
	unused_flag,
	overflow,
	negative
};

enum addressing_mode {
	relative,
	immediate,
	implied,
	accumulator,
	absolute,
	zeropage,
	absolute_indirect,
	absolute_x,
	absolute_y,
	zeropage_x,
	zeropage_y,
	zeropage_xi,
	zeropage_yi,
};

enum instruction_name {
	LDA, LDX, LDY, STA, STX, STY,
	ADC, SBC, INC, INX, INY, DEC,
	DEX, DEY, ASL, LSR, ROL, ROR,
	AND, ORA, EOR, CMP, CPX, CPY,
	BIT, BCC, BCS, BNE, BEQ, BPL,
	BMI, BVC, BVS, TAX, TXA, TAY,
	TYA, TSX, TXS, PHA, PLA, PHP,
	PLP, JMP, JSR, RTS, RTI, CLC,
	SEC, CLD, SED, CLI, SEI, CLV,
	BRK, NOP,
	unimplemented
};

typedef struct parsed_instruction {
	enum addressing_mode a;
	enum instruction_name n;
	char* m;
	byte o;
} Instruction;

typedef struct cpu_6502 {
	word pc;
	byte reg[5];
	bool running;
	bool branch_taken;
	char* current_instruction_name;
	Instruction current_instruction;
	byte oper[2];
} Cpu_6502;

void write_cpu_state (Cpu_6502* cpu, System system, FILE* f);
void print_addressing_mode(enum addressing_mode a);
Instruction parse(byte opcode);

void cpu_reset(Cpu_6502* cpu, System system);
void step(System system, Cpu_6502* cpu, struct parsed_instruction i, byte oper[2]);
void nmi(System system, Cpu_6502* cpu);

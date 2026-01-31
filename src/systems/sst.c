#include <stdio.h>
#include "../types.h"
#include "../bitmath.h"
#include "system.h"
#include "../chips/6502.h"
#include "sst.h"

byte mmap_sst(Sst* s, word addr, byte value, bool write)
{
	if (write) {
		s->ram[addr] = value;
		return 0;
	} else {
		return s->ram[addr];
	}
}

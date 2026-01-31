#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cjson/cJSON.h"
#include "types.h"
#include "systems/system.h"
#include "chips/6502.h"
#include "systems/sst.h"

FILE* dfh;
FILE* dfh2;

int run_test(int opcode, char* path);

int main(int argc, char* argv[])
{
	if (argc < 3) {
		return 1;
	}
	char logfilename[255];
	snprintf(logfilename, sizeof(logfilename), "logs/%02X.log", atoi(argv[2]));
	char debuglogfilename[255];
	snprintf(debuglogfilename, sizeof(logfilename), "logs/debug-%02X.log", atoi(argv[2]));

	dfh = fopen(debuglogfilename, "w");
	dfh2 = fopen(logfilename, "w");
	if (dfh == NULL || dfh2 == NULL) {
		printf("error opening logs\n");
		return 1;
	}
	run_test(atoi(argv[2]), argv[1]);
	fclose(dfh);
	fclose(dfh2);
	return 0;
}

int run_test(int opcode, char* path)
{
	Instruction ins = parse(opcode);
	if (ins.n == unimplemented) {
		printf("unimplemented opcode, skipping\n");
		return 0;
	}
	char filename[255];
	snprintf(filename, sizeof(filename), "%s/%02x.json", path, opcode);
	FILE* f = fopen(filename, "r");
	if (f == NULL) {
		printf("no file\n");
		return 1;
	}
	printf("testing %02X\n", opcode);
	char* jsonbuf;
	fseek(f, 0L, SEEK_END);
	size_t fsize = ftell(f);
	fseek(f, 0L, SEEK_SET);
	jsonbuf = malloc(sizeof(char) * fsize);
	fread(jsonbuf, sizeof(char), fsize, f);
	fclose(f);
	cJSON* test_json = cJSON_Parse(jsonbuf);
	free(jsonbuf);
	if (test_json == NULL) {
		printf("error parsing json\n");
		return 1;
	}
	Sst* sst = (Sst*)malloc(sizeof(Sst));
	sst->cpu = (Cpu_6502*)malloc(sizeof(Cpu_6502));
	System s;
	s.s = sst_system;
	s.h = sst;
	cpu_reset(sst->cpu, s);
	int tests_amount = cJSON_GetArraySize(test_json);
	cJSON* test_item;
	cJSON* test_name;
	cJSON* test_initial;
	cJSON* pc_initial;
	cJSON* a_initial;
	cJSON* x_initial;
	cJSON* y_initial;
	cJSON* p_initial;
	cJSON* sp_initial;
	cJSON* test_final;
	cJSON* pc_final;
	cJSON* a_final;
	cJSON* x_final;
	cJSON* y_final;
	cJSON* p_final;
	cJSON* sp_final;
	cJSON* test_ram_pokes;
	cJSON* test_ram_pokes_amount;
	for (int i; i<tests_amount; i++) {
		test_item = cJSON_GetArrayItem(test_json, i);
		test_name = cJSON_GetObjectItem(test_item, "name");
		test_initial = cJSON_GetObjectItem(test_item, "initial");
		test_final = cJSON_GetObjectItem(test_item, "final");
		test_ram_pokes = cJSON_GetObjectItem(test_initial, "ram");
		fprintf(dfh2, "running test '%s' (%d/%d)\n", test_name->valuestring, i+1, tests_amount);
		fprintf(dfh, "=========== %s ===========\n", test_name->valuestring);
		pc_initial = cJSON_GetObjectItem(test_initial, "pc");
		a_initial = cJSON_GetObjectItem(test_initial, "a");
		x_initial = cJSON_GetObjectItem(test_initial, "x");
		y_initial = cJSON_GetObjectItem(test_initial, "y");
		p_initial = cJSON_GetObjectItem(test_initial, "p");
		sp_initial = cJSON_GetObjectItem(test_initial, "s");
		pc_final = cJSON_GetObjectItem(test_final, "pc");
		a_final = cJSON_GetObjectItem(test_final, "a");
		x_final = cJSON_GetObjectItem(test_final, "x");
		y_final = cJSON_GetObjectItem(test_final, "y");
		p_final = cJSON_GetObjectItem(test_final, "p");
		sp_final = cJSON_GetObjectItem(test_final, "s");
		sst->cpu->pc = (word)pc_initial->valueint;
		sst->cpu->reg[reg_a] = (byte)a_initial->valueint;
		sst->cpu->reg[reg_x] = (byte)x_initial->valueint;
		sst->cpu->reg[reg_y] = (byte)y_initial->valueint;
		sst->cpu->reg[reg_p] = (byte)p_initial->valueint;
		sst->cpu->reg[reg_sp] = (byte)sp_initial->valueint;
		memset( sst->ram, 0, sizeof(byte) * sizeof(sst->ram) );
		cJSON* ram_poke;
		for (int ri=0; ri<cJSON_GetArraySize(test_ram_pokes); ri++) {
			word addr;
			byte value;
			cJSON* ram_addr;
			cJSON* ram_value;
			ram_poke = cJSON_GetArrayItem(test_ram_pokes, ri);
			ram_addr = cJSON_GetArrayItem(ram_poke, 0);
			ram_value = cJSON_GetArrayItem(ram_poke, 1);
			addr = (word)ram_addr->valuedouble;
			value = (byte)ram_value->valueint;
			mmap_sst(sst, addr, value, true);
			fprintf(dfh, "%X->%X\n", addr, value);
		}
		write_cpu_state(sst->cpu, s, dfh);
		ins = parse(opcode);
		byte oper[2] = {
			mmap_sst(sst, sst->cpu->pc+1, 0, false),
			mmap_sst(sst, sst->cpu->pc+2, 0, false),
		};
		step(s, sst->cpu, ins, oper);
		write_cpu_state(sst->cpu, s, dfh);
		cJSON* ram_final = cJSON_GetObjectItem(test_final, "ram");
		for (int ri2=0; ri2<cJSON_GetArraySize(ram_final); ri2++) {
			cJSON* ram_peek = cJSON_GetArrayItem(ram_final, ri2);
			cJSON* ram_addr;
			cJSON* ram_value;
			ram_addr = cJSON_GetArrayItem(ram_peek, 0);
			ram_value = cJSON_GetArrayItem(ram_peek, 1);
			word addr = (word)ram_addr->valueint;
			byte value = mmap_sst(sst, addr, 0, false);
			byte expected_value = (byte)ram_value->valueint;
			if ( value != expected_value ) {
				fprintf(dfh2, "%X doesn't match expected %X (%X)\n", addr, expected_value, value);
			} else {
				fprintf(dfh2, "%X matches expected %X\n", addr, expected_value);
			}
		}
		for (int ri3=0; ri3<6; ri3++) {
			word actual;
			word expected;
			char* reg;
			if (ri3 == 5) {
				actual = sst->cpu->pc;
			} else {
				actual = sst->cpu->reg[ri3];
			}
			switch (ri3) {
			case 0:
				expected = a_final->valueint;
				reg = "a";
				break;
			case 1:
				expected = x_final->valueint;
				reg = "x";
				break;
			case 2:
				expected = y_final->valueint;
				reg = "y";
				break;
			case 3:
				expected = sp_final->valueint;
				reg = "sp";
				break;
			case 4:
				expected = p_final->valueint;
				reg = "p";
				break;
			case 5:
				expected = pc_final->valueint;
				reg = "pc";
				break;
			}
			if (actual != expected) {
				fprintf(dfh2, "%s is not expected value %X (%X)\n", reg, expected, actual);
			} else {
				fprintf(dfh2, "%s is expected value %X\n", reg, expected);
			}
		}
		fprintf(dfh2, "=====================\n");
	}
	cJSON_Delete(test_json);
	free(sst->cpu);
	free(sst);
}

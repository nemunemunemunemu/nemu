enum systems {
	famicom_system,
	sst_system, // this is a dummy "system" that I used for checking the 6502 emulator against the single step tests
};

typedef struct system {
	enum systems s;
	void* h;
} System;

enum systems {
	famicom_system,
	apple1_system,
	sst_system,
};

typedef struct system {
	enum systems s;
	void* h;
} System;

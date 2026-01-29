enum systems {
	famicom_system,
	apple1_system,
};

typedef struct system {
	enum systems s;
	void* h;
} System;

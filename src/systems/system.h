enum systems {
	famicom_system,
};

typedef struct system {
	enum systems s;
	void* h;
} System;

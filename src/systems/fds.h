extern char fds_bios_path[1024];

int fds_load(Famicom* f, FILE* rom, byte* header, const char* path);
void fds_reset(Famicom* f, bool warm);
void fds_clock(Famicom* f);
byte fds_read(Famicom* f, word addr);
void fds_write(Famicom* f, word addr, byte value);
bool fds_irq(Famicom* f);
void fds_eject(Famicom* f);
void fds_insert(Famicom* f, int side);
void fds_switch_side(Famicom* f, int side);
void fds_save_disk(Famicom* f, const char* rom_path);

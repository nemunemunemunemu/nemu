// pulls the first rom out of a zip; returns a malloc'd buffer or NULL
unsigned char* zip_extract_rom(const char* path, int* size, char* name, int name_size);

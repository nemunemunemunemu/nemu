#define VRC7_RATE (3579545.0 / 72.0) // the opll makes a sample every 72 ticks of its 3.58MHz clock

void vrc7_reset(Famicom_vrc7* v);
void vrc7_write_address(Famicom_vrc7* v, byte value);
void vrc7_write_data(Famicom_vrc7* v, byte value);
void vrc7_clock(Famicom_vrc7* v, double cpu_hz);
float vrc7_channel_frequency(Famicom_vrc7* v, int channel);
float vrc7_channel_level(Famicom_vrc7* v, int channel);

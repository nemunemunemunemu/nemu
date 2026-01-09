#include <stdio.h>
#include "types.h"

byte set_bit ( byte b, int i, bool value )
{
	if (value == true) {
		b |= (1 << i); //set bit i
	} else {
		b &= ~(1 << (i)); //clear bit i
	}
	return b;
}

byte get_bit ( byte b, int i )
{
	return ((b >> i) & 0x01);
}

byte get_lower_byte ( word w )
{
	return (byte)(w & 0x00FF);
}

byte get_higher_byte ( word w )
{
	return (byte)(((w & 0xFF00) >> 8) & 0xff);
}

word bytes_to_word ( byte low, byte high )
{
	return (low << 8) | high;
}

void print_byte_as_bits(char val)
{
  for (int i = 7; 0 <= i; i--) {
    printf("%c", (val & (1 << i)) ? '1' : '0');
  }
}

byte reverse_byte_order(byte b)
{
   b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
   b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
   b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
   return b;
}


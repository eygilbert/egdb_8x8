#pragma once
#include <cstdio>


unsigned int crc_calc(char const *buf, int len);
unsigned int file_crc_calc(FILE *fp);
unsigned int file_crc_calc(FILE *fp, int *abort);
unsigned int file_crc_calc(FILE *fp);
int fname_crc_calc(char const *name, unsigned int *crc);

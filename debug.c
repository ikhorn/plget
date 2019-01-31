#include "debug.h"
#include <stdio.h>

void db_dump_buf(unsigned char *data, int size)
{
	char ascii[17];
	int i, j;

	ascii[16] = '\0';
	for (i = 0; i < size; ++i) {
		printf("%02X ", data[i]);

		if (data[i] >= ' ' && data[i] <= '~')
			ascii[i % 16] = data[i];
		else
			ascii[i % 16] = '.';

		if ((i + 1) % 8 == 0 || i + 1 == size) {
			printf(" ");

			if ((i + 1) % 16 == 0) {
				printf("|  %s \n", ascii);
			} else if (i + 1 == size) {
				ascii[(i + 1) % 16] = '\0';

				if ((i + 1) % 16 <= 8)
					printf(" ");

				for (j = (i + 1) % 16; j < 16; ++j)
					printf("   ");

				printf("|  %s \n", ascii);
			}
		}
	}
}

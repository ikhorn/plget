#include "debug.h"
#include <stdio.h>
#include <string.h>

void db_dump_buf(void *data, int size)
{
	unsigned char *arr = data;
	char ascii[17];
	int i, j;

	ascii[16] = '\0';
	for (i = 0; i < size; ++i) {
		printf("%02X ", arr[i]);

		if (arr[i] >= ' ' && arr[i] <= '~')
			ascii[i % 16] = arr[i];
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

#define PROGRESS_WIDTH				72

void pr_progress_bar(char label[], int step, int total)
{
	int width, pos, _pct, pct, pct_frag, i;

	width = PROGRESS_WIDTH - strlen(label); /* minus label len */
	pos = (step * width) / total ;

	_pct = (step * 10000) / total;
	pct = _pct / 100;
	pct_frag =  _pct - pct * 100;

	printf("%s[", label);

	/* fill progress bar with = */
	for (i = 0; i < pos; i++)
		printf( "%c", '=' );

	/* fill progress bar with spaces */
	printf("%*c", width - pos + 1, ']');
	printf(" %3d.%02d%%\r", pct, pct_frag);
}

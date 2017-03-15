#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "htczip.h"

int htc_largezip_read_header(int fd, htc_largezip_header_t *header)
{
    int pos = lseek(fd, 0, SEEK_CUR);

    if (read(fd, header, sizeof(htc_largezip_header_t)) != sizeof(htc_largezip_header_t)) {
		fprintf(stderr, "failed to read htc largezip header");
		lseek(fd, pos, SEEK_SET);
		return 0;
    }

    if(strncmp(header->magic, HTC_LARGEZIP_HEADER_MAGIC, strlen(HTC_LARGEZIP_HEADER_MAGIC))) {
    	lseek(fd, pos, SEEK_SET);
        return 0;
    }

    return 1;
}

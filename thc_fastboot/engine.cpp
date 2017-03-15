/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "fastboot.h"
#include "fs.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <ziparchive/zip_archive.h>
#include "htczip.h"

#define OP_DOWNLOAD   1
#define OP_COMMAND    2
#define OP_QUERY      3
#define OP_NOTICE     4
#define OP_DOWNLOAD_SPARSE 5
#define OP_WAIT_FOR_DISCONNECT 6

typedef struct Action Action;

#define CMD_SIZE 64

struct Action {
    unsigned op;
    Action* next;

    char cmd[CMD_SIZE];
    const char* prod;
    void* data;

    // The protocol only supports 32-bit sizes, so you'll have to break
    // anything larger into chunks.
    uint32_t size;

    const char *msg;
    int (*func)(Action* a, int status, const char* resp);

    double start;
};

static Action *action_list = 0;
static Action *action_last = 0;




bool fb_getvar(Transport* transport, const std::string& key, std::string* value) {
    std::string cmd = "getvar:";
    cmd += key;

    char buf[FB_RESPONSE_SZ + 1];
    memset(buf, 0, sizeof(buf));
    if (fb_command_response(transport, cmd.c_str(), buf)) {
      return false;
    }
    *value = buf;
    return true;
}

static int cb_default(Action* a, int status, const char* resp) {
    if (status) {
        fprintf(stderr,"FAILED (%s)\n", resp);
    } else {
        double split = now();
        fprintf(stderr,"OKAY [%7.3fs]\n", (split - a->start));
        a->start = split;
    }
    return status;
}

static Action *queue_action(unsigned op, const char *fmt, ...)
{
    va_list ap;
    size_t cmdsize;

    Action* a = reinterpret_cast<Action*>(calloc(1, sizeof(Action)));
    if (a == nullptr) die("out of memory");

    va_start(ap, fmt);
    cmdsize = vsnprintf(a->cmd, sizeof(a->cmd), fmt, ap);
    va_end(ap);

    if (cmdsize >= sizeof(a->cmd)) {
        free(a);
        die("Command length (%d) exceeds maximum size (%d)", cmdsize, sizeof(a->cmd));
    }

    if (action_last) {
        action_last->next = a;
    } else {
        action_list = a;
    }
    action_last = a;
    a->op = op;
    a->func = cb_default;

    a->start = -1;

    return a;
}

void fb_set_active(const char *slot)
{
    Action *a;
    a = queue_action(OP_COMMAND, "set_active:%s", slot);
    a->msg = mkmsg("Setting current slot to '%s'", slot);
}

void fb_queue_erase(const char *ptn)
{
    Action *a;
    a = queue_action(OP_COMMAND, "erase:%s", ptn);
    a->msg = mkmsg("erasing '%s'", ptn);
}

void fb_queue_flash(const char *ptn, void *data, unsigned sz)
{
    Action *a;

    a = queue_action(OP_DOWNLOAD, "");
    a->data = data;
    a->size = sz;
    a->msg = mkmsg("sending '%s' (%d KB)", ptn, sz / 1024);

    a = queue_action(OP_COMMAND, "flash:%s", ptn);
    a->msg = mkmsg("writing '%s'", ptn);
}

void fb_queue_flash_sparse(const char* ptn, struct sparse_file* s, unsigned sz, size_t current,
                           size_t total) {
    Action *a;

    a = queue_action(OP_DOWNLOAD_SPARSE, "");
    a->data = s;
    a->size = 0;
    a->msg = mkmsg("sending sparse '%s' %zu/%zu (%d KB)", ptn, current, total, sz / 1024);

    a = queue_action(OP_COMMAND, "flash:%s", ptn);
    a->msg = mkmsg("writing '%s' %zu/%zu", ptn, current, total);
}

static int match(const char* str, const char** value, unsigned count) {
    unsigned n;

    for (n = 0; n < count; n++) {
        const char *val = value[n];
        int len = strlen(val);
        int match;

        if ((len > 1) && (val[len-1] == '*')) {
            len--;
            match = !strncmp(val, str, len);
        } else {
            match = !strcmp(val, str);
        }

        if (match) return 1;
    }

    return 0;
}



static int cb_check(Action* a, int status, const char* resp, int invert)
{
    const char** value = reinterpret_cast<const char**>(a->data);
    unsigned count = a->size;
    unsigned n;
    int yes;

    if (status) {
        fprintf(stderr,"FAILED (%s)\n", resp);
        return status;
    }

    if (a->prod) {
        if (strcmp(a->prod, cur_product) != 0) {
            double split = now();
            fprintf(stderr,"IGNORE, product is %s required only for %s [%7.3fs]\n",
                    cur_product, a->prod, (split - a->start));
            a->start = split;
            return 0;
        }
    }

    yes = match(resp, value, count);
    if (invert) yes = !yes;

    if (yes) {
        double split = now();
        fprintf(stderr,"OKAY [%7.3fs]\n", (split - a->start));
        a->start = split;
        return 0;
    }

    fprintf(stderr,"FAILED\n\n");
    fprintf(stderr,"Device %s is '%s'.\n", a->cmd + 7, resp);
    fprintf(stderr,"Update %s '%s'",
            invert ? "rejects" : "requires", value[0]);
    for (n = 1; n < count; n++) {
        fprintf(stderr," or '%s'", value[n]);
    }
    fprintf(stderr,".\n\n");
    return -1;
}

static int cb_require(Action*a, int status, const char* resp) {
    return cb_check(a, status, resp, 0);
}

static int cb_reject(Action* a, int status, const char* resp) {
    return cb_check(a, status, resp, 1);
}

struct htc_zip {
	int zip_type;
	int current_flash;
	int open_fd;
	ZipArchiveHandle zip;
	void *current_data;
	htc_largezip_header_t largezip;
};

static struct htc_zip zip_info;
volatile int restart_usb = 0;

static int cb_multizip_check(Action* a, int status, const char* resp) {
	DEBUG("in %s - status %d\n", __func__, status);
    if (status) {
    	if (strstr(resp, "hboot pre-update") != NULL) {
    		DEBUG("got hboot pre-update - flash again - restart usb\n");
    		fb_queue_wait_for_disconnect();
    		sleep(5);
    		restart_usb = 1;
            fb_queue_flash_multizip(nullptr);
    		return 0;
    	}
        fprintf(stderr,"FAILED (%s)\n", resp);
        return status;
    }

    double split = now();
    fprintf(stderr,"OKAY [%7.3fs]\n", (split - a->start));
    zip_info.current_flash++;
    fb_queue_flash_multizip(nullptr);

    return 0;
}

static int cb_largezip_check(Action* a, int status, const char* resp) {
	DEBUG("in %s - status %d\n", __func__, status);
    if (status) {
    	if (strstr(resp, "hboot pre-update") != NULL) {
    		DEBUG("got hboot pre-update - flash again - restart usb\n");
    		fb_queue_wait_for_disconnect();
    		sleep(5);
    		restart_usb = 1;
            fb_queue_flash_largezip(nullptr);
    		return 0;
    	}
        fprintf(stderr,"FAILED (%s)\n", resp);
        return status;
    }

    double split = now();
    fprintf(stderr,"OKAY [%7.3fs]\n", (split - a->start));
    zip_info.current_flash++;
    fb_queue_flash_largezip(nullptr);

    return 0;
}

void fb_queue_flash_zip(const char *ptn, void *data, unsigned sz, int (*func)(Action* a, int status, const char* resp))
{
    Action *a;

    a = queue_action(OP_DOWNLOAD, "");
    a->data = data;
    a->size = sz;
//    a->func = cb_zip_check;
    a->msg = mkmsg("sending '%s' (%d KB)", ptn, sz / 1024);

    a = queue_action(OP_COMMAND, "flash:%s", "zip");
    //a->func = cb_zip_check;
    a->func = func;
    a->msg = mkmsg("writing '%s'", ptn);
}

void fb_queue_flash_largezip(const char *fname) {
	int c;
	ssize_t r;
	htc_largezip_header_t *header;
	char zipname[256];

	if (zip_info.open_fd == 0) {
		if (fname == nullptr) die("Should not happen - fname nullptr @ %s\n", __func__);
		zip_info.open_fd = open(fname, O_RDONLY);
		zip_info.current_data = NULL;
		zip_info.current_flash = 0;
		if (htc_largezip_read_header(zip_info.open_fd, &zip_info.largezip) == 0) {
			die("Failed to open largezip!!");
		}
	}

	c = zip_info.current_flash;
	if (c >= 8) return;

	header = &zip_info.largezip;

	DEBUG("Flashing largezips zip %d\n", c);

	if (header->lengths[c] <= 0) {
		close(zip_info.open_fd);
		return;
	}

	if (zip_info.current_data != NULL) {
		free(zip_info.current_data);
	}

	if (lseek64(zip_info.open_fd, header->starts[c], SEEK_SET) == -1) {
		die("Cannot seek to zip start (0x%08X) (%s)!!", header->starts[c], strerror(errno));
	}

	zip_info.current_data = malloc(header->lengths[c]);
	r = read(zip_info.open_fd, zip_info.current_data, header->lengths[c]);
	if (r != (int) header->lengths[c]) die("Yeah, fixme");

	memset(zipname, 0x00, 256);
	snprintf(zipname, 255, "%d-zip", zip_info.current_flash);
	DEBUG("flash zip from 0x%08X-0x%08X\n", header->starts[c], header->starts[c] + header->lengths[c]);
	fb_queue_flash_zip(zipname, zip_info.current_data, header->lengths[c], cb_largezip_check);


}

void fb_queue_flash_multizip(const char *fname) {
	int error;
	char zipname[256];
	int64_t sz;

	if (zip_info.open_fd == 0) {
		if (fname == nullptr) die("Should not happen - fname nullptr @ %s\n", __func__);
		zip_info.open_fd = open(fname, O_RDONLY);
		zip_info.current_data = NULL;
		zip_info.current_flash = 0;
		error = OpenArchiveFd(zip_info.open_fd, "", &zip_info.zip);
	    if (error != 0) {
	        CloseArchive(zip_info.zip);
	        die("Failed to open zip file in %s", __func__);
	    }
	}

	memset(zipname, 0x00, 256);
	snprintf(zipname, 255, "zip_%d.zip", zip_info.current_flash);
	DEBUG("Unzipping %s\n", zipname);

	if (zip_info.current_data != NULL) {
		free(zip_info.current_data);
	}
	zip_info.current_data = unzip_file(zip_info.zip, zipname, &sz);
	if (zip_info.current_data != nullptr) {
		DEBUG("Flashing %s %ld\n", zipname, (long int) sz);
		fb_queue_flash_zip(zipname, zip_info.current_data, sz, cb_multizip_check);
	}
	else {
		CloseArchive(zip_info.zip);
	}

}

void fb_queue_require(const char *prod, const char *var,
                      bool invert, size_t nvalues, const char **value)
{
    Action *a;
    a = queue_action(OP_QUERY, "getvar:%s", var);
    a->prod = prod;
    a->data = value;
    a->size = nvalues;
    a->msg = mkmsg("checking %s", var);
    a->func = invert ? cb_reject : cb_require;
    if (a->data == nullptr) die("out of memory");
}

static int cb_display(Action* a, int status, const char* resp) {
    if (status) {
        fprintf(stderr, "%s FAILED (%s)\n", a->cmd, resp);
        return status;
    }
    fprintf(stderr, "%s: %s\n", (char*) a->data, resp);
    return 0;
}

void fb_queue_display(const char *var, const char *prettyname)
{
    Action *a;
    a = queue_action(OP_QUERY, "getvar:%s", var);
    a->data = strdup(prettyname);
    if (a->data == nullptr) die("out of memory");
    a->func = cb_display;
}

static int cb_save(Action* a, int status, const char* resp) {
    if (status) {
        fprintf(stderr, "%s FAILED (%s)\n", a->cmd, resp);
        return status;
    }
    strncpy(reinterpret_cast<char*>(a->data), resp, a->size);
    return 0;
}

void fb_queue_query_save(const char *var, char *dest, unsigned dest_size)
{
    Action *a;
    a = queue_action(OP_QUERY, "getvar:%s", var);
    a->data = (void *)dest;
    a->size = dest_size;
    a->func = cb_save;
}

static int cb_do_nothing(Action*, int , const char*) {
    fprintf(stderr,"\n");
    return 0;
}

void fb_queue_reboot(void)
{
    Action *a = queue_action(OP_COMMAND, "reboot");
    a->func = cb_do_nothing;
    a->msg = "rebooting";
}

void fb_queue_command(const char *cmd, const char *msg)
{
    Action *a = queue_action(OP_COMMAND, cmd);
    a->msg = msg;
}

void fb_queue_download(const char *name, void *data, unsigned size)
{
    Action *a = queue_action(OP_DOWNLOAD, "");
    a->data = data;
    a->size = size;
    a->msg = mkmsg("downloading '%s'", name);
}

void fb_queue_notice(const char *notice)
{
    Action *a = queue_action(OP_NOTICE, "");
    a->data = (void*) notice;
}

void fb_queue_wait_for_disconnect(void)
{
    queue_action(OP_WAIT_FOR_DISCONNECT, "");
}

int fb_execute_queue(Transport* transport)
{
    Action *a;
    char resp[FB_RESPONSE_SZ+1];
    int status = 0;

    a = action_list;
    if (!a)
        return status;
    resp[FB_RESPONSE_SZ] = 0;

    double start = -1;
    for (a = action_list; a; a = a->next) {
    	if (restart_usb == 1) {
    		transport = reopen_device();
    		restart_usb = 0;
    	}
        a->start = now();
        if (start < 0) start = a->start;
        if (a->msg) {
            // fprintf(stderr,"%30s... ",a->msg);
            fprintf(stderr,"%s...\n",a->msg);
        }
        if (a->op == OP_DOWNLOAD) {
            status = fb_download_data(transport, a->data, a->size);
            status = a->func(a, status, status ? fb_get_error().c_str() : "");
            if (status) break;
        } else if (a->op == OP_COMMAND) {
            status = fb_command(transport, a->cmd);
            status = a->func(a, status, status ? fb_get_error().c_str() : "");
            if (status) break;
        } else if (a->op == OP_QUERY) {
            status = fb_command_response(transport, a->cmd, resp);
            status = a->func(a, status, status ? fb_get_error().c_str() : resp);
            if (status) break;
        } else if (a->op == OP_NOTICE) {
            fprintf(stderr,"%s\n",(char*)a->data);
        } else if (a->op == OP_DOWNLOAD_SPARSE) {
            status = fb_download_data_sparse(transport, reinterpret_cast<sparse_file*>(a->data));
            status = a->func(a, status, status ? fb_get_error().c_str() : "");
            if (status) break;
        } else if (a->op == OP_WAIT_FOR_DISCONNECT) {
            transport->WaitForDisconnect();
        } else {
            die("bogus action");
        }
    }

    fprintf(stderr,"finished. total time: %.3fs\n", (now() - start));
    return status;
}

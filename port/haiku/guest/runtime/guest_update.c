/*
GUEST_UPDATE.C

The guest's side of port/linux/src/update.h: HTTPS requests (the server
browser's master servers, a dedicated server's publishing, the updater's
downloads), which posix_update.c makes in the host with Mbed TLS. The
functions that take only strings and integers are imported as they are
(port/haiku/host_imports.list); these two carry what the ABIs lay out
differently: a callback, and struct update_response with its pointer and
its unsigned long.
*/

#include <stdlib.h>
#include <string.h>

#include "update.h"
#include "guest_host.h"

int update_download(const char *url, const char *path, unsigned long long maximum, update_progress_proc progress,
	void *context, char *error, int error_size)
{
	return host_update_download(url, path, maximum, (unsigned int)progress, (unsigned int)context, error,
		error_size);
}

int update_https_request(const char *method, const char *url, const char *headers, const void *body,
	unsigned long body_size, unsigned long maximum, struct update_response *response, char *error, int error_size)
{
	unsigned int size = 0;
	int status = 0;

	memset(response, 0, sizeof(*response));
	if (!host_update_https_request(method, url, headers, body, (unsigned int)body_size, (unsigned int)maximum,
		&status, response->headers, sizeof(response->headers), &size, error, error_size))
		return 0;
	response->status = status;
	response->body = malloc((size_t)size + 1);
	if (!response->body)
	{
		host_update_take_body(NULL);
		if (error_size > 0)
			strncpy(error, "out of memory", (size_t)error_size - 1)[error_size - 1] = 0;
		return 0;
	}
	host_update_take_body(response->body);
	response->body[size] = 0;
	response->body_size = size;
	return 1;
}

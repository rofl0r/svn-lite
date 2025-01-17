/*-
 * Copyright (c) 2012-2019, John Mehr <jmehr@umn.edu>
 * Copyright (c) 2021 rofl0r
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 *
 */

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/param.h> /* MAXNAMLEN */
#include <sys/tree.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/ssl3.h>
#include <openssl/err.h>
#include <openssl/md5.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <assert.h>

#include "stringlist.h"

#define SVNUP_VERSION "1.09"
#define BUFFER_UNIT 4096
#define COMMAND_BUFFER 32768
#define COMMAND_BUFFER_THRESHOLD 32000

#define LIT_LEN(S) (sizeof(S)-1)
#define starts_with_lit(S1, S2) \
	(strncmp(S1, S2, LIT_LEN(S2)) == 0)

typedef struct {
	int       socket_descriptor;
	enum      { NONE, SVN, HTTP, HTTPS } protocol;
	enum svn_job {
		SVN_NONE = 0,
		SVN_CO,
		SVN_LOG,
		SVN_INFO,
	} job;
	SSL      *ssl;
	SSL_CTX  *ctx;
	char     *address;
	uint16_t  port;
	uint32_t  revision;
	char     *commit_author;
	char     *commit_date;
	char     *commit_msg;
	int       family;
	char     *root;
	char     *trunk;
	char     *branch;
	char     *rev_root_stub;
	char     *path_target;
	char     *response;
	size_t    response_length;
	uint32_t  response_blocks;
	uint32_t  response_groups;
	char     *path_work;
	char     *known_files;
	char     *known_files_old;
	char     *known_files_new;
	long      known_files_size;
	int       trim_tree;
	int       extra_files;
	int       verbosity;
	char      inline_props;
} connector;


typedef struct {
	char      md5[33];
	char      md5_checked;
	char      download;
	char      executable;
	char      special;
	char     *href;
	char     *path;
	uint64_t  raw_size;
	int64_t   size;
} file_node;


struct tree_node {
	RB_ENTRY(tree_node)  link;
	char                *md5;
	char                *path;
};


/* Function Prototypes */

static char		*md5sum(void*, size_t, char*);
static int		 tree_node_compare(const struct tree_node *, const struct tree_node *);
static void		 prune(connector *, char *);
static char		*find_response_end(int, char *, char *);
static void		 find_local_files_and_directories(char *, const char *, int);
static void		 reset_connection(connector *);
static void		 send_command(connector *, const char *);
static int		 check_command_success(int, char **, char **);
static char		*process_command_svn(connector *, const char *, unsigned int);
static char		*process_command_http(connector *, char *);
static char		*parse_xml_value(char *, char *, const char *);
static void		 parse_response_group(connector *, char **, char **);
static int		 parse_response_item(connector *, char *, int *, char **, char **);
static file_node	*new_file_node(file_node ***, int *, int *);
static int		 save_file(char *, char *, char *, int, int);
static void		 save_known_file_list(connector *, file_node **, int);
static void		 create_directory(char *);
static void		 process_report_svn(connector *, char *, file_node ***, int *, int *);
static void		 process_report_http(connector *, file_node ***, int *file_count, int *);
static void		 parse_additional_attributes(connector *, char *, char *, file_node *);
static void		 get_files(connector *, char *, char *, file_node **, int, int);
static void		 progress_indicator(connector *connection, char *, int, int);

/* turn svn date string like "2020-11-10T09:23:51.711212Z" into "2020-11-10 09:23:51" */
static char* sanitize_svn_date(char *date) {
	char *temp = temp;
	temp = strchr(date, 'T');
	*temp++ = ' ';
	temp = strchr(temp, '.');
	*temp = 'Z';
	*temp = 0;
	return temp;
}

static char*
http_extract_header_value(char* response, const char* name, char* buf, size_t buflen)
{
	char *line, *p;
	line = p = response;
	size_t l = strlen(name);
	while(1) {
		p = strstr(p, "\r\n");
		if(!p) break;
		if(!strncmp(line, name, l) && line[l] == ':' && line[l+1] == ' ') {
			intptr_t ll = p - line - l - 2;
			if(ll > 0 && ll+1 < buflen) {
				memcpy(buf, line + l + 2, ll);
				buf[ll] = 0;
				return buf;
			}
			return 0;
		}
		p += 2;
		line = p;
	}
	return 0;
}

static char* strip_rev_root_stub(connector *connection, char* path) {
	char *tmp = path;
	if(connection->rev_root_stub && strstr(path, connection->rev_root_stub) == path) {
		tmp += strlen(connection->rev_root_stub);
		if(*tmp == '/')++tmp;
		while(isdigit(*tmp))++tmp;
	}
	return tmp;
}

/*
 * md5sum
 *
 * Function that returns hexadecimal md5 hash in out.
 * out needs to be char[MD5_DIGEST_LENGTH*2+1].
 */
static char*
md5sum(void *data, size_t dlen, char *out)
{
	MD5_CTX       md5_context;
	unsigned char md5_digest[MD5_DIGEST_LENGTH];
	size_t i, j;

	MD5_Init(&md5_context);
	MD5_Update(&md5_context, data, dlen);
	MD5_Final(md5_digest, &md5_context);

	for (i = 0, j = 0; i < MD5_DIGEST_LENGTH; i++, j+=2)
		sprintf(out+j, "%02x", md5_digest[i]);

	return out;
}

/*
 * tree_node_compare
 *
 * Function that informs the Red-Black tree functions how to sort keys.
 */

static int
tree_node_compare(const struct tree_node *a, const struct tree_node *b)
{
	return (strcmp(a->path, b->path));
}

/*
 * tree_node_free
 *
 * Function that frees the memory used by a tree node.
 */

static void
tree_node_free(struct tree_node *node)
{
	if (node->md5 != NULL)
		free(node->md5);

	if (node->path != NULL)
		free(node->path);

	free(node);
}


static RB_HEAD(tree_known_files, tree_node) known_files = RB_INITIALIZER(&known_files);
RB_PROTOTYPE(tree_known_files, tree_node, link, tree_node_compare)
RB_GENERATE(tree_known_files, tree_node, link, tree_node_compare)

static RB_HEAD(tree_local_files, tree_node) local_files = RB_INITIALIZER(&local_files);
RB_PROTOTYPE(tree_local_files, tree_node, link, tree_node_compare)
RB_GENERATE(tree_local_files, tree_node, link, tree_node_compare)

static RB_HEAD(tree_local_directories, tree_node) local_directories = RB_INITIALIZER(&local_directories);
RB_PROTOTYPE(tree_local_directories, tree_node, link, tree_node_compare)
RB_GENERATE(tree_local_directories, tree_node, link, tree_node_compare)


/*
 * prune
 *
 * Procedure that removes the file passed in (and it's parent folder if it's empty).
 */

static void
prune(connector *connection, char *path_target)
{
	struct stat  local;
	char        *temp, *temp_file;
	size_t       length;

	length = strlen(path_target) + strlen(connection->path_target) + 2;

	if ((temp_file = (char *)malloc(length)) == NULL)
		err(EXIT_FAILURE, "prune temp_file malloc");

	snprintf(temp_file, length, "%s%s", connection->path_target, path_target);

	if (stat(temp_file, &local) != -1) {
		if (connection->verbosity)
			printf(" - %s\n", temp_file);

		if ((S_ISREG(local.st_mode)) || (S_ISLNK(local.st_mode))) {
			if (remove(temp_file) != 0) {
				err(EXIT_FAILURE, "Cannot remove %s", temp_file);
			} else {
				/* Isolate the parent directory in the path name
				 * and try and remove it.  Failure is ok. */

				if ((temp = strrchr(temp_file, '/')) != NULL) {
					*temp = '\0';
					rmdir(temp_file);
				}
			}
		}

		if (S_ISDIR(local.st_mode))
			rmdir(temp_file);
	}

	free(temp_file);
}


/*
 * find_local_files_and_directories
 *
 * Procedure that recursively finds and adds local files and directories to
 * separate red-black trees.
 */

static void
find_local_files_and_directories(char *path_base, const char *path_target, int include_files)
{
	DIR              *dp;
	struct stat       local;
	struct dirent    *de;
	struct tree_node *data;
	char             *temp_file;
	size_t            length, len;

	length = strlen(path_base) + strlen(path_target) + MAXNAMLEN + 3;

	if ((temp_file = (char *)malloc(length)) == NULL)
		err(EXIT_FAILURE, "find_local_files_and_directories temp_file malloc");

	snprintf(temp_file, length, "%s%s", path_base, path_target);

	if (lstat(temp_file, &local) != -1) {
		if (S_ISDIR(local.st_mode)) {

			/* Keep track of the local directories, ignoring path_base. */

			if (strlen(path_target)) {
				data = (struct tree_node *)malloc(sizeof(struct tree_node));
				data->path = strdup(temp_file);
				data->md5 = NULL;

				RB_INSERT(tree_local_directories, &local_directories, data);
			}

			/* Recursively process the contents of the directory. */

			if ((dp = opendir(temp_file)) != NULL) {
				while ((de = readdir(dp)) != NULL) {
					len = strlen(de->d_name);

					if ((len == 1) && (strcmp(de->d_name, "." ) == 0))
						continue;

					if ((len == 2) && (strcmp(de->d_name, "..") == 0))
						continue;

					snprintf(temp_file,
						length,
						"%s/%s",
						path_target,
						de->d_name);

					find_local_files_and_directories(path_base, temp_file, include_files);
				}

				closedir(dp);
			}
		} else {
			if (include_files) {
				data = (struct tree_node *)malloc(sizeof(struct tree_node));
				data->path = strdup(path_target);
				data->md5 = NULL;

				RB_INSERT(tree_local_files, &local_files, data);
			}
		}
	}

	free(temp_file);
}


/*
 * find_response_end
 *
 * Function that locates the end of a command response in the response stream.  For the SVN
 * protocol, it counts opening and closing parenthesis and for HTTP/S, it looks for a pair
 * of CRLFs.
 */

static char *
find_response_end(int protocol, char *start, char *end)
{
	int count = 0;

	if (protocol == SVN)
		do {
			count += (*start == '(' ? 1 : (*start == ')' ? -1 : 0));
		}
		while ((*start != '\0') && (start++ < end) && (count > 0));

	if (protocol >= HTTP)
		start = strstr(start, "\r\n\r\n") + 4;

	return (start);
}


/*
 * reset_connection
 *
 * Procedure that (re)establishes a connection with the server.
 */

static void
reset_connection(connector *connection)
{
	struct addrinfo hints = {
		.ai_family = connection->family,
		.ai_socktype = SOCK_STREAM,
	}, *start, *temp, *gai;
	int             error, option;
	char            type[10];

	if (connection->socket_descriptor != -1)
		if (close(connection->socket_descriptor) != 0)
			if (errno != EBADF) err(EXIT_FAILURE, "close_connection");

	snprintf(type, sizeof(type), "%d", connection->port);

	if ((error = getaddrinfo(connection->address, type, &hints, &start)))
		errx(EXIT_FAILURE, "%s", gai_strerror(error));

	gai = start;

	connection->socket_descriptor = -1;
	while (start) {
		temp = start;

		if (connection->socket_descriptor < 0) {
			if ((connection->socket_descriptor = socket(temp->ai_family, temp->ai_socktype, temp->ai_protocol)) < 0)
				err(EXIT_FAILURE, "socket failure");

			if (connect(connection->socket_descriptor, temp->ai_addr, temp->ai_addrlen) < 0)
				err(EXIT_FAILURE, "connect failure");
		}

		start = temp->ai_next;
	}

	if(gai) freeaddrinfo(gai);

	if (connection->protocol == HTTPS) {
		if (SSL_library_init() == 0)
			err(EXIT_FAILURE, "reset_connection: SSL_library_init");

		SSL_load_error_strings();
		connection->ctx = SSL_CTX_new(SSLv23_client_method());
		SSL_CTX_set_mode(connection->ctx, SSL_MODE_AUTO_RETRY);
		SSL_CTX_set_options(connection->ctx, SSL_OP_ALL | SSL_OP_NO_TICKET);

		if ((connection->ssl = SSL_new(connection->ctx)) == NULL)
			err(EXIT_FAILURE, "reset_connection: SSL_new");

		SSL_set_fd(connection->ssl, connection->socket_descriptor);
		while ((error = SSL_connect(connection->ssl)) == -1)
			fprintf(stderr, "SSL_connect error:%d\n", SSL_get_error(connection->ssl, error));
	}

	option = 1;

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_KEEPALIVE, &option, sizeof(option)))
		err(EXIT_FAILURE, "setsockopt SO_KEEPALIVE error");

	option = COMMAND_BUFFER;

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_SNDBUF, &option, sizeof(option)))
		err(EXIT_FAILURE, "setsockopt SO_SNDBUF error");

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_RCVBUF, &option, sizeof(option)))
		err(EXIT_FAILURE, "setsockopt SO_RCVBUF error");
}


/*
 * send_command
 *
 * Procedure that sends commands to the http/svn server.
 */

static void
send_command(connector *connection, const char *command)
{
	size_t  bytes_to_write, total_bytes_written;
	ssize_t bytes_written;

	if (command) {
		total_bytes_written = 0;
		bytes_to_write = strlen(command);

		if (connection->verbosity > 2)
			fprintf(stdout, "<< %zu bytes\n%s", bytes_to_write, command);

		while (total_bytes_written < bytes_to_write) {
			if (connection->protocol == HTTPS)
				bytes_written = SSL_write(
					connection->ssl,
					command + total_bytes_written,
					bytes_to_write - total_bytes_written);
			else
				bytes_written = write(
					connection->socket_descriptor,
					command + total_bytes_written,
					bytes_to_write - total_bytes_written);

			if (bytes_written <= 0) {
				if ((bytes_written < 0) && ((errno == EINTR) || (errno == 0))) {
					continue;
				} else {
					err(EXIT_FAILURE, "send command");
				}
			}

			total_bytes_written += bytes_written;
		}
	}
}


/*
 * check_command_success
 *
 * Function that makes sure a failure response has not been sent from the svn server.
 */

static int
check_command_success(int protocol, char **start, char **end)
{
	int   fail = 0;
	char *response = *start;

	if (protocol == SVN) {
		if (starts_with_lit(*start, "( success ( ( ) 0: ) ) ( failure"))
			fail = 1;

		else if (starts_with_lit(*start, "( success ( ) ) ( failure"))
			fail = 1;

		if (!fail) {
			while (**start == ' ') (*start)++;

			if (starts_with_lit(*start, "( success ")) {
				if (starts_with_lit(*start, "( success ( ( ) 0: ) )"))
					*start += LIT_LEN("( success ( ( ) 0: ) )")+1;
				*end = find_response_end(protocol, *start, *end) + 1;
			}
			else fail = 1;
		}
	}

	if (protocol >= HTTP) {
		if (!starts_with_lit(*start, "HTTP/1.1 "))
			fail = 1;
		else {
			*start += LIT_LEN("HTTP/1.1 ");
			if(**start != '2') fail = 1;
			else {
				*start = strstr(*start, "\r\n\r\n");
				if (*start) *start += 4; else fail = 1;
			}
		}
	}

	if (fail) {
		if(protocol == SVN || !strstr(response, "xml version="))
			fprintf(stderr, "\nCommand Failure: %s\n", response);
		else {
			char *ec = parse_xml_value(*start, *end, "m:human-readable");
			if(ec) { fprintf(stderr, "\n%s\n", ec); free(ec); }
			else fprintf(stderr, "\nCommand Failure: %s\n", response);
		}
	}

	return (fail);
}


/*
 * process_command_svn
 *
 * Function that sends a command set to the svn server and parses its response to make
 * sure that the expected number of response strings have been received.
 */

static char *
process_command_svn(connector *connection, const char *command, unsigned int expected_bytes)
{
	ssize_t       bytes_read;
	int           count, ok;
	unsigned int  group, position, try;
	char         *check, input[BUFFER_UNIT + 1];

	try = 0;
	retry:

	send_command(connection, command);

	count = position = ok = group = connection->response_length = 0;

	do {
		bzero(input, BUFFER_UNIT + 1);

		bytes_read = read(connection->socket_descriptor, input, BUFFER_UNIT);

		if (bytes_read <= 0) {
			if ((errno == EINTR) || (errno == 0)) continue;

			if (++try > 5)
				errx(EXIT_FAILURE, "Error in svn stream.  Quitting.");

			if (try > 1)
				fprintf(stderr, "Error in svn stream, retry #%d\n", try);

			goto retry;
		}

		input[bytes_read] = 0;
		if (connection->verbosity > 3)
			fprintf(stdout, "<< %s\n", input);

		connection->response_length += bytes_read;

		/* always allocate at least BUFFER_UNIT bytes more than we actually need
		   because there's some wacky code in get_files() - see FIXME comment
		   before the memmove() there */
		size_t max = connection->response_length + BUFFER_UNIT;
		if(expected_bytes + BUFFER_UNIT > max) max = expected_bytes + BUFFER_UNIT;
		if (max >= connection->response_blocks * BUFFER_UNIT) {
			do connection->response_blocks += (connection->response_blocks/2);
			while(max >= connection->response_blocks * BUFFER_UNIT);

			connection->response = realloc(
				connection->response,
				connection->response_blocks * BUFFER_UNIT + 1);

			if (connection->response == NULL)
				err(EXIT_FAILURE, "process_command_svn realloc");
		}

		if (expected_bytes == 0) {
			if (input[1] == '\0') {
				connection->response[position++] = input[0];
				continue;
			}

			if (connection->verbosity > 3)
				fprintf(stdout, "==========\n>> Response Parse:\n");

			check = input;
			if ((count == 0) && (input[0] == ' '))
				*check++ = '\0';

			do {

				if (*check == ')')
					--count;
				else if(*check == '(') {
					/* trying to skip size-annotated block, which in case of
					   a commit message may contain unbalanced parens */
					int skip = 0;
					char *q, *p = check+1, *e = input + bytes_read;
					if(p+2 < e && p[0] == ' ' && isdigit(p[1])) {
						q = p+1;
						while(q < e && isdigit(*q)) ++q;
						if(q < e && *q == ':') {
							skip = atoi(p)+1;
							check = q;
						}
					}
					if(skip) {
						if(check+skip < input + bytes_read) {
							check += skip;
						} else if (connection->verbosity > 3) {
							fprintf(stderr, "couldn't skip %d bytes", skip);
						}
					}
					++count;
				}

				if (connection->verbosity > 3)
					fprintf(stderr, "%d", count);

				if (count == 0) {
					group++;
					check++;
					if (check < input + bytes_read) {
						if (*check == ' ')
							*check = '\0';

						if (*check != '\0')
							fprintf(stderr, "oops: %d %c\n", *check, *check);

						char *q = check + 1;
						while(q < input + bytes_read && *q != '(') ++q;
						check = q-1;
					}
				}
			}
			while (++check < input + bytes_read);
		}

		memcpy(connection->response + position, input, bytes_read + 1);
		position += bytes_read;

		if ((expected_bytes == 0) && (connection->verbosity > 3))
			fprintf(stderr, ". = %d %d\n", group, connection->response_groups);

		if (group >= connection->response_groups)
			ok = 1;

		if (position == expected_bytes)
			ok = 1;

		if ((expected_bytes > 0) && (connection->response[0] == ' ') && (position == expected_bytes + 1))
			ok = 1;
	}
	while (!ok);

	if ((expected_bytes == 0) && (connection->verbosity > 2))
		fprintf(stdout, "==========\n>> Response:\n%s", connection->response);

	connection->response[position] = '\0';

	return (connection->response);
}


/*
 * process_command_http
 *
 * Function that sends a command set to the http server and parses its response to make
 * sure that the expected number of response bytes have been received.
 */

static char *
process_command_http(connector *connection, char *command)
{
	int           bytes_read, chunk, chunked_transfer, first_chunk, gap, read_more, spread;
	unsigned int  groups, offset, try;
	char         *begin, *end, input[BUFFER_UNIT + 1], *marker1, *marker2, *temp, hex_chunk[32];

	try = 0;
	retry:

	chunked_transfer = -1;
	connection->response_length = chunk = groups = 0;
	offset = read_more = 0;
	first_chunk = 1;
	begin = end = marker1 = marker2 = temp = NULL;

	bzero(connection->response, connection->response_blocks * BUFFER_UNIT + 1);
	bzero(input, BUFFER_UNIT + 1);

	if (try || connection->socket_descriptor == -1)
		reset_connection(connection);
	send_command(connection, command);

	while (groups < connection->response_groups) {
		spread = connection->response_length - offset;

		if (spread <= 0)
			read_more = 1;

		/* Sometimes the read returns only part of the next offset, so
		 * if there were less than five bytes read, keep reading to get
		 * the remainder of the offset. */

		if ((chunked_transfer == 1) && (spread <= 5))
			read_more = 1;

		if ((chunked_transfer == 0) && (spread == 0) && (connection->response_groups - groups == 1))
			break;

		if (read_more) {
			if (connection->protocol == HTTPS)
				bytes_read = SSL_read(
					connection->ssl,
					input,
					BUFFER_UNIT);
			else
				bytes_read = read(
					connection->socket_descriptor,
					input,
					BUFFER_UNIT);

			if (connection->response_length + bytes_read > connection->response_blocks * BUFFER_UNIT) {
				while(connection->response_length + bytes_read > connection->response_blocks * BUFFER_UNIT)
					connection->response_blocks += (connection->response_blocks/2);

			#define SAVE_VAR(X) \
				intptr_t X ## _offset; \
				int was_null_ ## X = 0; \
				if(X) X ## _offset = X - connection->response; \
				else was_null_ ## X = 1;

				SAVE_VAR(marker2);
				SAVE_VAR(begin);
				SAVE_VAR(end);

				connection->response = (char *)realloc(
					connection->response,
					connection->response_blocks * BUFFER_UNIT + 1);

				if (connection->response == NULL)
					err(EXIT_FAILURE, "process_command_http realloc");

			#define RESTORE_VAR(X) \
				if (!was_null_ ## X) \
					X = connection->response + X ## _offset;

				RESTORE_VAR(marker2);
				RESTORE_VAR(begin);
				RESTORE_VAR(end);
			}

			if (bytes_read < 0) {
				if ((errno == EINTR) || (errno == 0))
					continue;

			check_tries_and_retry:;
				if (++try > 5)
					errx(EXIT_FAILURE, "Error in http stream.  Quitting.");

				if (try > 1)
					fprintf(stderr, "Error in http stream, retry #%d\n", try);

				goto retry;
			}

			if (bytes_read == 0) {
				if(connection->response_length == 0) goto check_tries_and_retry;
				break;
			}

			memcpy(connection->response + connection->response_length, input, bytes_read + 1);
			connection->response_length += bytes_read;
			connection->response[connection->response_length] = '\0';
			read_more = 0;
			spread = connection->response_length - offset;
		}

		if ((chunked_transfer == 0) && (spread >= 0)) {
			chunked_transfer = -1;
			groups++;
		}

		if (chunked_transfer == -1) {
			begin = connection->response + offset;

			if ((begin = strstr(begin, "HTTP/1.1 ")) == NULL) {
				read_more = 1;
				continue;
			}

			if ((end = strstr(begin, "\r\n\r\n")) == NULL) {
				read_more = 1;
				continue;
			}

			if(strstr(begin, "DAV: http://subversion.tigris.org/xmlns/dav/svn/inline-props"))
				connection->inline_props = 1;

			end += 4;

			offset += (end - begin);
			groups++;

			marker1 = strstr(begin, "Content-Length: ");
			marker2 = strstr(begin, "Transfer-Encoding: chunked");

			if (marker1) chunked_transfer = 0;
			if (marker2) chunked_transfer = 1;

			if ((marker1) && (marker2))
				chunked_transfer = (marker1 < marker2) ? 0 : 1;

			if (chunked_transfer == 0) {
				chunk = strtol(marker1 + 16, (char **)NULL, 10);

				if (chunk < 0)
					errx(EXIT_FAILURE, "process_command_http: Bad stream data");

				offset += chunk;
				if (connection->response_length > offset) {
					chunked_transfer = -1;
					groups++;
				}
			}

			if (chunked_transfer == 1) {
				chunk = 0;
				marker2 = end;
			}
		}

		while ((chunked_transfer == 1) && ((end = strstr(marker2, "\r\n")) != NULL)) {
			chunk = strtol(marker2, (char **)NULL, 16);
			marker2 -= 2;

			if (chunk < 0)
				errx(EXIT_FAILURE, "process_command_http: Bad stream data ");

			snprintf(hex_chunk, sizeof(hex_chunk), "\r\n%x\r\n", chunk);
			gap = strlen(hex_chunk);

			if (marker2 + chunk + gap > connection->response + connection->response_length) {
				marker2 += 2;
				read_more = 1;
				break;
			}

			if (first_chunk) {
				first_chunk = 0;
				chunk += gap;
			}
			else {
				/* Remove the chunk from the buffer. */

				memmove(marker2,
					marker2 + gap,
					connection->response_length - (marker2 - connection->response));

				connection->response_length -= gap;
			}

			/* Move the offset to the end of the chunk. */

			offset += chunk;
			marker2 += chunk + 2;

			if (chunk == 0) {
				chunked_transfer = -1;
				groups++;
			}
		}

		if (connection->verbosity > 2)
			fprintf(stderr, "\rBytes read: %zd, Bytes expected: %d, g:%d, rg:%d",
				connection->response_length,
				offset,
				groups,
				connection->response_groups);
	}

	if (connection->verbosity > 2)
		fprintf(stderr, "\rBytes read: %zd, Bytes expected: %d, g:%d, rg:%d",
			connection->response_length,
			offset,
			groups,
			connection->response_groups);

	if (connection->verbosity > 2)
		fprintf(stderr, "\n");

	if (connection->verbosity > 3)
		fprintf(stderr, "==========\n%s\n==========\n", connection->response);

	if(!strstr(connection->response, "HTTP/1.1 "))
		errx(EXIT_FAILURE, "unexpected response from HTTP server:\n%s", connection->response);

	return (connection->response);
}


/*
 * parse_xml_value
 *
 * Function that returns the text found between the opening and closing tags passed in.
 */

static char *
parse_xml_value(char *start, char *end, const char *tag)
{
	size_t  tag_length;
	char   *data_end, *data_start, *end_tag, temp_end, *value;

	value = NULL;
	temp_end = *end;
	*end = '\0';

	tag_length = strlen(tag) + 4;
	if ((end_tag = (char *)malloc(tag_length)) == NULL)
		err(EXIT_FAILURE, "parse_xml_value end_tag malloc");

	snprintf(end_tag, tag_length, "</%s>", tag);

	if ((data_start = strstr(start, tag))) {
		if ((data_start = strchr(data_start, '>'))) {
			data_start++;
			data_end = strstr(data_start, end_tag);

			if (data_end) {
				if ((value = (char *)malloc(data_end - data_start + 1)) == NULL)
					err(EXIT_FAILURE, "parse_xml_value value malloc");

				memcpy(value, data_start, data_end - data_start);
				value[data_end - data_start] = '\0';
			}
		}
	}

	free(end_tag);
	*end = temp_end;

	return (value);
}


/*
 * parse_response_group
 *
 * Procedure that isolates the next response group from the response stream.
 */

static void
parse_response_group(connector *connection, char **start, char **end)
{
	if (connection->protocol == SVN)
		*end = find_response_end(connection->protocol, *start, *end);

	if (connection->protocol >= HTTP) {
		*end = strstr(*start, "</D:multistatus>");
		if (*end != NULL) *end += 16;
		else errx(EXIT_FAILURE, "Error in http stream: %s\n", *start);
	}

	**end = '\0';
}


/*
 * parse_response_item
 *
 * Function that isolates the next response from the current response group.
 */

static int
parse_response_item(connector *connection, char *end, int *count, char **item_start, char **item_end)
{
	int c, has_entries, ok;

	c = has_entries = 0;
	ok = 1;

	if (connection->protocol == SVN) {
		if (*count == '\0') {
			while ((c < 3) && (*item_start < end)) {
				c += (**item_start == '(' ? 1 : (**item_start == ')' ? -1 : 0));
				if (**item_start == ':') has_entries++;
				(*item_start)++;
			}

			(*item_start) += 5;
			*item_end = *item_start;
		}

		c = 1;
		(*item_end)++;

		while ((c > 0) && (*item_end < end)) {
			(*item_end)++;
			c += (**item_end == '(' ? 1 : (**item_end == ')' ? -1 : 0));
			if (**item_end == ':') has_entries++;
		}

		(*item_end)++;
		**item_end = '\0';
	}

	if (connection->protocol >= HTTP) {
		*item_end = strstr(*item_start, "</D:response>");

		if (*item_end != NULL) {
			*item_end += 13;
			**item_end = '\0';
			has_entries = 1;
		} else ok = 0;
	}

	if (!has_entries) ok = 0;

	(*count)++;

	return (ok);
}

/* checking whether the md5 of file as retrieved from online repo
   matches the value in known_files list. if the file isn't in
   known_files then there's no point checking against the
   filesystem either, as it shouldn't exist there.
   the only case where it could be interesting to check against
   a filesystem copy would be the case of a huge repo checked
   out with a different client (i.e. no known_files file exists
   yet), and the user being unwilling to check
   out every single file again - but in that case he could just
   produce the file known_files with a script that runs md5sum
   over all files.
*/
static void check_md5(connector *connection, file_node *file) {
	char buf[4096];
	struct tree_node  *data, *next;
	if(file->md5[0] && !file->md5_checked) {
		file->md5_checked = 1;
		file->download = 1; /* default to "md5 doesn't match local file" */
		snprintf(buf, sizeof buf, "%s",
			strip_rev_root_stub(connection, file->path));

		for (data = RB_MIN(tree_known_files, &known_files); data != NULL; data = next) {
			if(!strcmp(data->path, buf)) {
				if(!memcmp(data->md5, file->md5, 32)) {
					file->download = 0;
					return;
				}
				/* file encountered in known_files, but md5 mismatch */
				break;
			}
			next = RB_NEXT(tree_known_files, head, data);
		}
	}
}

/*
 * new_file_node
 *
 * Function that allocates a new file_node and expands the dynamic
 * array that stores file_nodes.
 */

static file_node *
new_file_node(file_node ***file, int *file_count, int *file_max)
{
	file_node *node = calloc(1, sizeof(file_node));

	if (node == NULL)
		err(EXIT_FAILURE, "new_file_node node malloc");

	(*file)[*file_count] = node;

	if (++(*file_count) == *file_max) {
		*file_max += BUFFER_UNIT;

		if ((*file = (file_node **)realloc(*file, *file_max * sizeof(file_node **))) == NULL)
			err(EXIT_FAILURE, "new_file_node file realloc");
	}

	return (node);
}

/*
 * save_file
 *
 * Procedure that saves a file.
 */

static int
save_file(char *filename, char *start, char *end, int executable, int special)
{
	struct stat  local;
	int          fd, saved;
	char        *tag;

	saved = 0;

	if (special) {
		if (starts_with_lit(start, "link ")) {
			*end = '\0';

			if (stat(filename, &local) == 0)
				if (remove(filename) != 0)
					errx(EXIT_FAILURE, "Please remove %s manually and restart svnup", filename);

			if (symlink(start + 5, filename)) {
				err(EXIT_FAILURE, "Cannot link %s -> %s", start + 5, filename);

			saved = 1;
			}
		}
	} else {
		if ((fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, executable ? 0755 : 0644)) == -1)
			err(EXIT_FAILURE, "write file failure %s", filename);

		write(fd, start, end - start);
		close(fd);

		saved = 1;
	}

	return (saved);
}


/*
 * save_known_file_list
 *
 * Procedure that saves the list of files known to be in the repository.
 */

static void
save_known_file_list(connector *connection, file_node **file, int file_count)
{
	struct tree_node  find, *found;
	int               fd, x;

	if ((fd = open(connection->known_files_new, O_WRONLY | O_CREAT | O_TRUNC, 0644)) == -1)
		err(EXIT_FAILURE, "write file failure %s", connection->known_files_new);

	for (x = 0; x < file_count; x++) {
		write(fd, file[x]->md5, strlen(file[x]->md5));
		write(fd, "\t", 1);
		char* ftmp = strip_rev_root_stub(connection, file[x]->path);
		write(fd, ftmp, strlen(ftmp));
		write(fd, "\n", 1);

		/* If the file exists in the red-black trees, remove it. */

		find.path = ftmp;

		if ((found = RB_FIND(tree_known_files, &known_files, &find)) != NULL)
			tree_node_free(RB_REMOVE(tree_known_files, &known_files, found));

		if ((found = RB_FIND(tree_local_files, &local_files, &find)) != NULL)
			tree_node_free(RB_REMOVE(tree_local_files, &local_files, found));

		if (file[x]->href)
			free(file[x]->href);

		free(file[x]->path);
		free(file[x]);
		file[x] = NULL;
	}

	close(fd);
}


static int protocol_from_str(char* line, connector *connection) {
	if (strncmp(line, "svn", 3) == 0) {
		connection->protocol = SVN;
		connection->port = 3690;
	} else

	if (strncmp(line, "https", 5) == 0) {
		connection->protocol = HTTPS;
		connection->port = 443;
	} else

	if (strncmp(line, "http", 4) == 0) {
		connection->protocol = HTTP;
		connection->port = 80;
	} else {
		connection->protocol = NONE;
		return 0;
	}

	return 1;
}

/*
 * create_directory
 *
 * Procedure that checks for and creates a local directory if possible.
 */

static void
create_directory(char *directory)
{
	struct stat  local;
	int          create;

	create = stat(directory, &local);

	if (create == 0) {
		/* If a file exists with the same name, try and remove it first. */

		if (!S_ISDIR(local.st_mode)) {
			if (remove(directory) != 0)
				err(EXIT_FAILURE, "%s exists and is not a directory.  Please remove it manually and restart svnup", directory);
			else
				create = 1;
		}
	}

	if (create)
		if (mkdir(directory, 0755))
			err(EXIT_FAILURE, "Cannot create %s", directory);
}


/* concats all strings in stringlist (up to a defined margin) into a single
   long string, and removes the processed entries from the list.
   returns pointer to a freshly allocated string.
   returns NULL if no strings are left.
   if items is set to non-zero, it is used as a max value for the number
   of items concatenated.
   items will be filled with the number of strings joined into the result.*/
static char *concat_stringlist(stringlist *sl, size_t maxlen, size_t *items) {
	char *chain = malloc(maxlen),
	     *cp = chain, *ce = chain+(maxlen-1);
	size_t max_items = *items;
	*items = 0;
	while(stringlist_getsize(sl)) {
		char *s = stringlist_get(sl, 0);
		size_t l = strlen(s);
		if(cp + l < ce && (!max_items || *items < max_items)) {
			strcpy(cp, s);
			cp += l;
			++(*items);
			stringlist_delete(sl, 0);
			free(s);
		} else {
			return chain;
		}
	}
	if(*items == 0) { free(chain); chain = 0; }
	return chain;
}

/*
 * process_report_svn
 *
 * Procedure that sends the svn report command and saves the initial details
 * in a dynamic array of file_nodes.
 */

static void
process_report_svn(connector *connection, char *command, file_node ***file, int *file_count, int *file_max)
{
	file_node   *this_file;
	struct tree_node  *found, find;
	struct stat  local;
	int          count, path_exists, try, x;
	size_t       d, length, name_length, path_length, path_source_length;
	char        *command_start, *directory_end, *directory_start, *end;
	char        *item_end, *item_start, *marker, *name;
	char        *path_source, *start, *temp;
	stringlist *buffered_commands = stringlist_new(16);

	path_source = malloc(MAXNAMLEN + 1);

	try = -1;

	retry:

	start = process_command_svn(connection, command, 0);
	end   = start + connection->response_length;

	command_start = command;

	directory_start = command_start;

	for (d = 0; d < connection->response_groups / 2; d++) {
		if (!starts_with_lit(directory_start, "( get-dir ( "))
			errx(EXIT_FAILURE, "Error in response: %s\n", directory_start);

		directory_end = strchr(directory_start, '\n');

		temp = strchr(directory_start, ':') + 1;
		directory_start = strchr(temp, ' ');

		length = directory_start - temp;
		if (length > 0)
			memcpy(path_source, temp, length);

		path_source[length] = '\0';
		path_source_length = length;

		directory_start = directory_end + 1;

		/* Parse the response for file/directory names. */

		end = connection->response + connection->response_length;
		if (check_command_success(connection->protocol, &start, &end)) {
			if (++try > 5)
				errx(EXIT_FAILURE, "Error in svn stream.  Quitting.");

			if (try > 1)
				fprintf(stderr, "Error in svn stream, retry #%d\n", try);

			goto retry;
		}

		parse_response_group(connection, &start, &end);

		item_start = start;
		item_end = end;

		count = 0;

		while (parse_response_item(connection, end, &count, &item_start, &item_end)) {
			temp = NULL;

			/* Keep track of the remote files. */

			length = strtol(item_start + 1, (char **)NULL, 10);
			if (length > MAXNAMLEN)
				errx(EXIT_FAILURE, "entry_is_file file name is too long");

			marker = strchr(item_start, ':') + 1 + length;

			if (starts_with_lit(marker, " file ")) {
				this_file = new_file_node(file, file_count, file_max);

				name_length = strtol(item_start + 1, (char **)NULL, 10);
				if (name_length > MAXNAMLEN)
					errx(EXIT_FAILURE, "process_file_entry file name is too long");

				name = item_start = strchr(item_start, ':') + 1;

				item_start += name_length;
				*item_start = '\0';
				path_length = strlen(path_source) + name_length + 2;

				if (!starts_with_lit(item_start + 1, "file "))
					errx(EXIT_FAILURE, "process_file_entry malformed response");

				if ((this_file->path = (char *)malloc(path_length)) == NULL)
					err(EXIT_FAILURE, "process_file_entry file->path malloc");

				snprintf(this_file->path, path_length, "%s/%s", path_source, name);

				item_start = strchr(item_start + 1, ' ');
				this_file->size = strtol(item_start, (char **)NULL, 10);
			}

			if (starts_with_lit(marker, " dir ")) {
				length = strtol(item_start + 1, (char **)NULL, 10);
				if (length > MAXNAMLEN)
					errx(EXIT_FAILURE, "process_file file name is too long");

				name = strchr(item_start, ':') + 1;
				name[length] = '\0';

				char *temp_path = malloc(BUFFER_UNIT+1);

				snprintf(temp_path,
					BUFFER_UNIT,
					"%s%s/%s",
					connection->path_target,
					path_source,
					name);

				/* Create the directory locally if it doesn't exist. */

				path_exists = stat(temp_path, &local);

				if ((path_exists != -1) && (!S_ISDIR(local.st_mode)))
					errx(EXIT_FAILURE, "%s exists locally and is not a directory.  Please remove it manually and restart svnup", temp_path);

				if (path_exists == -1) {
					if (connection->verbosity)
						printf(" + %s\n", temp_path);

					if (mkdir(temp_path, 0755) && errno != EEXIST)
						err(EXIT_FAILURE, "Cannot create target directory");
				}

				/* Remove the directory from the local directory tree to avoid later attempts at pruning. */

				find.path = temp_path;

				if ((found = RB_FIND(tree_local_directories, &local_directories, &find)) != NULL)
					tree_node_free(RB_REMOVE(tree_local_directories, &local_directories, found));

				/* Add a get-dir command to the command buffer. */

				length += path_source_length + 1;

				char* next_command = malloc(BUFFER_UNIT+1);
				snprintf(next_command,
					BUFFER_UNIT,
					"( get-dir ( %zd:%s/%s ( %d ) false true ( kind size ) false ) )\n",
					length,
					path_source,
					name,
					connection->revision);

				stringlist_add_dup(buffered_commands, next_command);
				free(next_command);
				free(temp_path);
			}

			item_start = item_end + 1;
		}

		start = end + 1;
	}

	/* Recursively process the command buffers. */
	/* we pack up to 32KB worth of file requests into a single buffer */
	char *chain;
	size_t chain_count = 0;
	while((chain = concat_stringlist(buffered_commands, BUFFER_UNIT, &chain_count))) {
		connection->response_groups = 2 * chain_count;
		process_report_svn(connection, chain, file, file_count, file_max);
		free(chain);
	}
	stringlist_free(buffered_commands);
	free(path_source);
}

static char* craft_http_packet(const char *host, const char* url,
	const char* verb, const char* footer, char* command) {
	snprintf(command,
		COMMAND_BUFFER,
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: svnup-%s\r\n"
		"Content-Type: text/xml\r\n"
		"Connection: Keep-Alive\r\n"
		"DAV: http://subversion.tigris.org/xmlns/dav/svn/depth\r\n"
		"DAV: http://subversion.tigris.org/xmlns/dav/svn/mergeinfo\r\n"
		"DAV: http://subversion.tigris.org/xmlns/dav/svn/log-revprops\r\n"
		"Transfer-Encoding: chunked\r\n\r\n"
		"%lx\r\n"
		"%s"
		"\r\n0\r\n\r\n",
		verb, url, host,
		SVNUP_VERSION,
		strlen(footer),
		footer);
	return command;
}

/* expects pointer to a string like "( 6: foobar )", puts "foobar" into saveloc (heap)
   returns pointer to after ')' */
static char* extract_svn_string_from_group(char* data, char** saveloc) {
	char *p = data;
	assert(*p == '(');
	++p;
	assert(*p == ' ');
	++p;
	assert(isdigit(*p));
	int n = atoi(p);
	while(isdigit(*p))++p;
	assert(*p == ':');
	++p;
	*saveloc = malloc(n+1);
	memcpy(*saveloc, p, n);
	(*saveloc)[n] = 0;
	p += n + 1;
	assert(*p == ')');
	return ++p;
}


static void process_log_svn(connector *connection) {
	char command[COMMAND_BUFFER + 1], *start, *end;

	snprintf(command, COMMAND_BUFFER,
		 "( log ( ( 0: ) ( %d ) ( %d ) false false 0 false revprops"
		 " ( 10:svn:author 8:svn:date 7:svn:log ) ) ) ",
		 connection->revision, connection->revision);

	connection->response_groups = 2; /* 3 on success, 2 on error */
	process_command_svn(connection, command, 0);
	start = connection->response;
	end = connection->response + connection->response_length;

	char* group2 = connection->response + strlen(connection->response) +1;
	if(group2 < end && starts_with_lit(group2, "done ( failure ( ( "))
		errx(EXIT_FAILURE, "%s", group2 + LIT_LEN("done ( failure ( ( "));

	if (check_command_success(connection->protocol, &start, &end))
		errx(EXIT_FAILURE, "couldn't get log");

	char buf[32], *p, *date;
	snprintf(buf, sizeof buf, " %d ( ", connection->revision);
	p = strstr(start, buf);
	if(!p) return;

	p += strlen(buf)-2;

	p = extract_svn_string_from_group(p, &connection->commit_author);
	assert(*p == ' ');
	++p;

	p = extract_svn_string_from_group(p, &connection->commit_date);
	assert(*p == ' ');
	++p;

	p = extract_svn_string_from_group(p, &connection->commit_msg);
	assert(*p == ' ');

	sanitize_svn_date(connection->commit_date);
}

static void process_log_http(connector *connection) {
	char command[COMMAND_BUFFER + 1], footer[1024], url[512];

	snprintf(url, sizeof url,
		"%s/%d",
		connection->rev_root_stub,
		connection->revision
	);
	snprintf(footer, sizeof footer,
		"<S:log-report xmlns:S=\"svn:\">"
			"<S:start-revision>%d</S:start-revision>"
			"<S:end-revision>%d</S:end-revision>"
			"<S:revprop>svn:author</S:revprop>"
			"<S:revprop>svn:date</S:revprop>"
			"<S:revprop>svn:log</S:revprop>"
			"<S:path></S:path>"
			"<S:encode-binary-props></S:encode-binary-props>"
		"</S:log-report>\r\n"
		,
		connection->revision,
		connection->revision
	);

	craft_http_packet(connection->address, url, "REPORT", footer, command);
	connection->response_groups = 2;

	process_command_http(connection, command);

	char *start = connection->response,
	     *end = start + connection->response_length, *p;

	if(check_command_success(connection->protocol, &start, &end))
		errx(EXIT_FAILURE, "couldn't get log\n%s", start);

	if((p = strstr(start, "xml version="))) start = p+10;

	connection->commit_author = parse_xml_value(start, end, "D:creator-displayname");
	connection->commit_date   = parse_xml_value(start, end, "S:date");
	connection->commit_msg    = parse_xml_value(start, end, "D:comment");
	if(connection->commit_date)
		sanitize_svn_date(connection->commit_date);
	else
		fprintf(stderr, "warning: empty reply for log request\n");
}

/*
 * process_report_http
 *
 * Procedure that sends the http report command and saves the initial details
 * in a dynamic array of file_nodes.
 */

static void
process_report_http(connector *connection, file_node ***file, int *file_count, int *file_max)
{
	file_node   *this_file;
	struct tree_node  *found, find;
	char         command[COMMAND_BUFFER + 1], *d, *end, *href, *md5, *path;
	char        *start, *temp, temp_buffer[BUFFER_UNIT], *value;
	char footer[512];

	connection->response_groups = 2;

	snprintf(footer, sizeof footer,
		"<S:update-report xmlns:S=\"svn:\">"
			"%s"
			"<S:src-path>/%s</S:src-path>"
			"<S:target-revision>%d</S:target-revision>"
			"<S:depth>unknown</S:depth>"
			"<S:entry rev=\"%d\" depth=\"infinity\" start-empty=\"true\"></S:entry>"
		"</S:update-report>\r\n"
		,
		connection->inline_props ? "<S:include-props>yes</S:include-props>" : "",
		connection->branch,
		connection->revision,
		connection->revision
	);

	char url[256];
	snprintf(url, sizeof url, "/%s/!svn/me", connection->root);

	craft_http_packet(connection->address, url, "REPORT", footer, command);

	process_command_http(connection, command);

	/* Process response for subdirectories and create them locally. */

	start = connection->response;
	end   = connection->response + connection->response_length;

	int has_inline_props = !!strstr(start, "inline-props=\"true\">");
	connection->inline_props = has_inline_props;

	while ((start = strstr(start, "<S:add-directory")) && (start < end)) {
		value = parse_xml_value(start, end, "D:href");
		char *ptmp = strip_rev_root_stub(connection, value);
		temp = strstr(ptmp, connection->trunk) + strlen(connection->trunk);
		snprintf(temp_buffer, BUFFER_UNIT, "%s%s", connection->path_target, temp);

		/* If a file exists with the same name, try and remove it first. */
/*
		if (stat(temp_buffer, &local) == 0)
			if (S_ISDIR(local.st_mode) == 0)
				if (remove(temp_buffer) != 0)
					err(EXIT_FAILURE, "Please remove %s manually and restart svnup", temp_buffer);
*/
		if(mkdir(temp_buffer, 0755) && errno != EEXIST)
			err(EXIT_FAILURE, "failed to create directory %s", temp_buffer);
		free(value);
		start++;

		/* Remove the directory from the local directory tree to avoid later attempts at pruning. */

		find.path = temp_buffer;

		if ((found = RB_FIND(tree_local_directories, &local_directories, &find)) != NULL)
			tree_node_free(RB_REMOVE(tree_local_directories, &local_directories, found));
	}

	start = connection->response;

	while ((start = strstr(start, "<S:add-file")) && (start < end)) {
		this_file = new_file_node(file, file_count, file_max);

		char *file_end = strstr(start, "</S:add-file>");
		if(file_end) file_end += LIT_LEN("</S:add-file>");
		else file_end = end;
		if(has_inline_props) {
			temp = strstr(start, "<S:set-prop name=\"svn:executable\">*</S:set-prop>");
			if(temp && temp < file_end)
				this_file->executable = 1;
			temp = strstr(start, "<S:set-prop name=\"svn:special\">*</S:set-prop>");
			if(temp && temp < file_end)
				this_file->special = 1;
			this_file->size = -1;
		}
		md5  = parse_xml_value(start, file_end, "V:md5-checksum");
		href = parse_xml_value(start, file_end, "D:href");
		if(connection->trunk[0] == 0)
			temp = href;
		else
			temp = strstr(href, connection->trunk);
		temp += strlen(connection->trunk);
		path = strdup(temp);

		/* Convert any hex encoded characters in the path. */

		d = path;
		while ((d = strchr(d, '%')) != NULL)
			if ((isxdigit(d[1])) && (isxdigit(d[2]))) {
				d[1] = toupper(d[1]);
				d[2] = toupper(d[2]);
				*d = ((isalpha(d[1]) ? 10 + d[1] -'A' : d[1] - '0') << 4) +
				      (isalpha(d[2]) ? 10 + d[2] -'A' : d[2] - '0');
				memmove(d + 1, d + 3, strlen(path) - (d - path + 2));
				d++;
			}

		this_file->href = href;
		this_file->path = path;
		memcpy(this_file->md5, md5, 32);

		start = file_end;
	}
}


/*
 * parse_additional_attributes
 *
 * Procedure that extracts md5 signature plus last author, committed date
 * and committed rev and saves them for later inclusion in revision tags.
 */

static void
parse_additional_attributes(connector *connection, char *start, char *end, file_node *file)
{
	char *md5, *temp, *value;

	if (file == NULL) return;

	if (connection->protocol == SVN) {
		if ((temp = strchr(start, ':')) != NULL) {
			md5 = ++temp;
			memcpy(file->md5, md5, 32);

			file->executable = (strstr(start, "14:svn:executable") ? 1 : 0);
			file->special    = (strstr(start, "11:svn:special") ? 1 : 0);
		}
	} else if (connection->protocol >= HTTP) {
		value = parse_xml_value(start, end, "lp1:getcontentlength");
		file->size = strtol(value, (char **)NULL, 10);
		free(value);

		file->executable = (strstr(start, "<S:executable/>") ? 1 : 0);
		file->special    = (strstr(start, "<S:special>*</S:special>") ? 1 : 0);
	}
}

static int get_content_length(const char *start, const char *end, size_t *len) {
	char *p = strstr(start, "Content-Length: ");
	if(!p || p > end) return 0;
	*len = strtol(p + LIT_LEN("Content-Length: "), NULL, 10);
	return 1;
}

/*
 * get_files
 *
 * Procedure that extracts and saves files from the response stream.
 */

static void
get_files(connector *connection, char *command, char *path_target, file_node **file, int file_start, int file_end)
{
	int     try, x, block_size, block_size_markers, file_block_remainder;
	int     first_response, last_response, offset, position, raw_size, saved;
	char   *begin, *end, file_path_target[BUFFER_UNIT], *gap, *start, *temp_end;
	char    md5_check[33];

	/* Calculate the number of bytes the server is going to send back. */

	try = 0;
	retry:
	if (try) reset_connection(connection);

	raw_size = 0;

	if (connection->protocol >= HTTP) {
		process_command_http(connection, command);

		start = connection->response;

		for (x = file_start; x <= file_end; x++) {
			if ((file[x] == NULL) || (file[x]->download == 0))
				continue;

			if(start == connection->response + connection->response_length)
				goto increment_tries;

			if(!(end = strstr(start, "\r\n\r\n")))
				goto increment_tries;

			if(file[x]->size == -1LL) {
				size_t ns;
				if(!get_content_length(start, end, &ns))
					errx(EXIT_FAILURE, "failed to extract Content-Length!");
				file[x]->size = ns;
			}
			end += 4;
			file[x]->raw_size = file[x]->size + (end - start);
			start = end + file[x]->size;
			raw_size += file[x]->raw_size;
		}
	}

	if (connection->protocol == SVN) {
		last_response  = 20;
		first_response = 84;

		x = connection->revision;
		while ((int)(x /= 10) > 0)
			first_response++;

		for (x = file_start; x <= file_end; x++) {
			if ((file[x] == NULL) || (file[x]->download == 0))
				continue;

			block_size_markers = 6 * (int)(file[x]->size / BUFFER_UNIT);
			if (file[x]->size % BUFFER_UNIT)
				block_size_markers += 3;

			file_block_remainder = file[x]->size % BUFFER_UNIT;
			while ((int)(file_block_remainder /= 10) > 0)
				block_size_markers++;

			file[x]->raw_size = file[x]->size +
				first_response +
				last_response +
				block_size_markers;

			raw_size += file[x]->raw_size;
		}

		process_command_svn(connection, command, raw_size);
	}

	/* Process the response stream and extract each file. */

	position = raw_size;

	for (x = file_end; x >= file_start; x--) {
		if (file[x]->download == 0)
			continue;

		char *tmp = strip_rev_root_stub(connection, file[x]->path);

		snprintf(file_path_target,
			BUFFER_UNIT,
			"%s%s",
			path_target,
			tmp);

		/* Isolate the file from the response stream. */

		end = connection->response + position;
		start = end - file[x]->raw_size;
		begin = end - file[x]->size;
		temp_end = end;

		if (check_command_success(connection->protocol, &start, &temp_end)) {
		increment_tries:;
			if (++try > 5)
				errx(EXIT_FAILURE, "Error in get_files.  Quitting.");

			if (try > 1)
				fprintf(stderr, "Error in get files, retry #%d\n", try);

			goto retry;
		}

		if (connection->protocol == SVN) {
			start = find_response_end(connection->protocol, start, temp_end) + 1;
			begin = strchr(start, ':') + 1;
			block_size = strtol(start, (char **)NULL, 10);
			offset = 0;
			start = begin;

			while (block_size == BUFFER_UNIT) {
				start += block_size + offset;
				gap = start;
				start = strchr(gap, ':') + 1;
				block_size = strtol(gap, (char **)NULL, 10);
		/* FIXME this memmove here was identified as writing memory
		   outside the allocated area (if the aread allocated was very close to
		   the actually transfered content size); probably by as many
		   bytes as it tries to move the block around, which is identical to the SVN
		   header for that block. */
				memmove(gap, start, file[x]->raw_size - (start - begin) + 1);
				offset = gap - start;
			}
		}

		/*
		 * Check to make sure the MD5 signature of the file in the buffer
		 * matches what the svn server originally reported.
		 */

		if (connection->verbosity > 1)
			printf("\r\e[0K\r");

		/* Make sure the MD5 checksums match before saving the file. */

		if (strncmp(file[x]->md5, md5sum(begin, file[x]->size, md5_check), 33) != 0) {
			begin[file[x]->size] = '\0';
			errx(EXIT_FAILURE, "MD5 checksum mismatch: should be %s, calculated %s\n", file[x]->md5, md5_check);
		}

		saved = save_file(file_path_target,
				begin,
				begin + file[x]->size,
				file[x]->executable,
				file[x]->special);

		if ((saved) && (connection->verbosity))
			printf(" + %s\n", file_path_target);

		position -= file[x]->raw_size;
		bzero(connection->response + position, file[x]->raw_size);
	}
}


/*
 * progress_indicator
 *
 * Procedure that neatly prints the current file and its position in the file list as a percentage.
 */

static void
progress_indicator(connector *connection, char *path, int f, int file_count)
{
	struct winsize window;
	int            file_width, term_width, x;
	char          *columns, file_path_target[BUFFER_UNIT], temp_buffer[BUFFER_UNIT];

	term_width = -1;
	file_width = 2;

	x = file_count;
	while ((int)(x /= 10) > 0)
		file_width++;

	/* Figure out the width of the terminal window. */

	if (isatty(STDERR_FILENO)) {
		if (((columns = getenv("COLUMNS")) != NULL) && (*columns != '\0'))
			term_width = strtol(columns, (char **)NULL, 10);
		else {
			if ((ioctl(STDERR_FILENO, TIOCGWINSZ, &window) != -1) && (window.ws_col > 0))
				term_width = window.ws_col;
		}
	}

	snprintf(file_path_target,
		BUFFER_UNIT,
		"%s%s",
		connection->path_target,
		path);

	/* If the width of the window is smaller than the output line, trim
	 * off the beginning of the path. */

	x = (term_width == -1) ? 1 : 0;
	if (15 + 2 * file_width + strlen(file_path_target) < (unsigned int)term_width)
		x = 1;

	snprintf(temp_buffer,
		BUFFER_UNIT,
		"% *d of %d (% 5.1f%%)  %s%s\e[0K\r",
		file_width,
		f + 1,
		file_count,
		100.0 * f / (double)file_count,
		(x ? "" : "..."),
		file_path_target + (x ? 0 : strlen(file_path_target) - term_width + file_width + file_width + 18));

	fprintf(stderr, "%s", temp_buffer);
}

static void
usage_svn(char *arg0) {
	fprintf(stderr,
		"svn-lite version %s by John Mehr & rofl0r\n\n"
		"Usage: svn command [options] [args]\n\n"
		"commands:\n\n"
		"info [options] TARGET\n"
		"   print some information about TARGET.\n"
		"   TARGET may either be an URL or a local directory.\n\n"
		"log [options] TARGET\n"
		"   print commit log of TARGET\n"
		"   TARGET may either be an URL or a local directory.\n\n"
		"checkout/co [options] URL [PATH]\n"
		"   checkout repository (equivalent to git clone/git pull).\n"
		"   if PATH is omitted, basename of URL will be used as destination\n"
		"\n"
		"options applicable to all commands:\n"
		"   -r or --revision   NUMBER (default: 0)\n"
		"   -v or --verbosity  NUMBER (default: 1)\n"
		, SVNUP_VERSION
	);
	exit(EXIT_FAILURE);
}

static int has_revision_option(enum svn_job mode) {
	switch(mode) {
	case SVN_INFO: case SVN_CO: case SVN_LOG:
		return 1;
	}
	return 0;
}

static char *protocol_check(char *str, connector *connection)
{
	char *p = strchr(str, ':');
	if(!p || p[1] != '/' || p[2] != '/') {
		connection->protocol = NONE;
		return str;
	}
	if(!protocol_from_str(str, connection)) {
		errx(EXIT_FAILURE, "unknown protocol %s\n", str);
	}
	p += 3;
	return p;
}


static void
getopts_svn(int argc, char **argv, connector *connection)
{
	int a = 1;

	if(argc < 2) usage_svn(argv[0]);
	if(!strcmp(argv[a], "checkout") || !strcmp(argv[a], "co"))
		connection->job = SVN_CO;
	else if(!strcmp(argv[a], "info"))
		connection->job = SVN_INFO;
	else if(!strcmp(argv[a], "log"))
		connection->job = SVN_LOG;
	else
		usage_svn(argv[0]);
	++a;
	while(1) {
		int opt = 0;
		if(!strcmp(argv[a], "-r") || !strcmp(argv[a], "--revision"))
			opt = 1;
		else if(!strcmp(argv[a], "-v") || !strcmp(argv[a], "--verbosity"))
			opt = 2;
		if(!opt) break;
		if(opt == 1 && !has_revision_option(connection->job))
			usage_svn(argv[0]);
		++a;
		if(a >= argc) usage_svn(argv[0]);
		int n = atoi(argv[a++]);
		if(opt == 1) connection->revision = n;
		else if(opt == 2) connection->verbosity = n;
		if(a >= argc) usage_svn(argv[0]);
	}

	char *p = protocol_check(argv[a], connection), *q, *dst;
	if(connection->job == SVN_CO && connection->protocol == NONE)
		usage_svn(argv[0]);
	if(connection->protocol != NONE) {
		if((q = strchr(p, ':'))) {
			connection->port = atoi(q+1);
			*q = 0;
			connection->address = strdup(p);
			*q = ':';
		} else if((q = strchr(p, '/'))) {
			*q = 0;
			connection->address = strdup(p);
			*q = '/';
		}
		if(q && *q == ':') q = strchr(q, '/');
		if(!q) {
			err(EXIT_FAILURE, "expected '/' in URL!");
		}
		p = ++q;
		connection->branch = strdup(p);
		if(connection->job == SVN_CO) {
			if(++a >= argc)
				dst = basename(connection->branch);
			else
				dst = argv[a];
			connection->path_target = strdup(dst);
		}
	} else {
		connection->path_target = strdup(argv[a]);
	}
	if(++a < argc) usage_svn(argv[0]);

	if(connection->path_target) {
		char buf[PATH_MAX];
		snprintf(buf, sizeof buf, "%s/.svnup", connection->path_target);
		connection->path_work = strdup(buf);
	}

	connection->trim_tree = 1;
}

static void write_info_or_log(connector *connection) {
	if(connection->job == SVN_LOG) {
		char deco[73];
		memset(deco, '-', 72);
		deco[72] = 0;
		fprintf(stdout, "%s\n", deco);
		/* some broken svn repos have empty revisions, and svn log prints only a
		   single line of decorations, e.g.

		   user@~$ svn log -r 70 svn://repo.hu/genht/trunk
		   ------------------------------------------------------------------------
		   user@~$

		   in case of svn info, the last "good" rev is displayed as "Last Changed Rev"

		   user@~$ svn info -r 70 svn://repo.hu/genht/trunk
		   Path: trunk
		   URL: svn://repo.hu/genht/trunk
		   Relative URL: ^/trunk
		   Repository Root: svn://repo.hu/genht
		   Repository UUID: 050b73db-cdb7-47b8-9107-1ae054b27eea
		   Revision: 70
		   Node Kind: directory
		   Last Changed Author: igor2
		   Last Changed Rev: 69
		   Last Changed Date: 2017-06-27 07:06:39 +0000 (Tue, 27 Jun 2017)
		   user@~$
		*/
		if(connection->commit_author) {
			fprintf(stdout, "r%u | %s | %s |\n\n", connection->revision, connection->commit_author, connection->commit_date);
			fprintf(stdout, "%s\n%s\n", connection->commit_msg, deco);
		}
	} else if(connection->job == SVN_INFO) {
		fprintf(stdout, "Revision: %u\n", connection->revision);
		if(connection->commit_author) {
			fprintf(stdout, "Last Changed Author: %s\n", connection->commit_author);
			fprintf(stdout, "Last Changed Rev: %u\n", connection->revision);
			fprintf(stdout, "Last Changed Date: %s +0000\n", connection->commit_date);
		}
	} else {
		assert(0);
	}
}

static const char* protocol_to_string(int proto) {
	static const char proto_strmap[][6] = {
		[SVN] = "svn", [HTTP] = "http", [HTTPS] = "https",
	};
	return proto >= SVN && proto <= HTTPS ? proto_strmap[proto] : NULL;
}

static void save_revision_file(connector *connection, char *svn_version_path) {
	FILE *f;
	if (!(f = fopen(svn_version_path, "w")))
		err(EXIT_FAILURE, "write file failure %s", svn_version_path);
	const char *ps = protocol_to_string(connection->protocol);
	fprintf(f, "rev=%u\n", connection->revision);
	fprintf(f, "url=%s://%s/%s\n", ps, connection->address, connection->branch);
	fprintf(f, "date=%s\n", connection->commit_date ? connection->commit_date : "");
	fprintf(f, "author=%s\n", connection->commit_author ? connection->commit_author : "");
	fprintf(f, "log=%s\n", connection->commit_msg ? connection->commit_msg : "");
	fclose(f);
	chmod(svn_version_path, 0644);
}

static void read_revision_file(connector *connection, char *svn_version_path) {
	FILE *f = fopen(svn_version_path, "r");
	char buf[1024];
	int in_log = 0;
	if(!f) errx(EXIT_FAILURE, "couldn't open %s", svn_version_path);
	while(fgets(buf, sizeof buf, f)) {
		if(in_log) {
			size_t l = strlen(connection->commit_msg);
			connection->commit_msg = realloc(connection->commit_msg, l + 1 + sizeof buf);
			if(l && connection->commit_msg[l-1] == '\n');
			else { connection->commit_msg[l] = '\n'; ++l; }
			char *p = strchr(buf, '\n');
			if(p) *p = 0;
			memcpy(connection->commit_msg+l, buf, strlen(buf)+1);
		} else if(!strncmp(buf, "rev=", 4)) {
			unsigned rev = atoi(buf+4);
			if(connection->revision && connection->revision != rev)
				errx(EXIT_FAILURE, "no local date for selected revision available, got %u", rev);
			connection->revision = rev;
		} else if(!strncmp(buf, "date=", 5)) {
			char *p = strchr(buf+5, '\n');
			if(!p) errx(EXIT_FAILURE, "malformed file %s", svn_version_path);
			*p = 0;
			p = buf + 5;
			if(*p)
				connection->commit_date = strdup(p);
		} else if(!strncmp(buf, "author=", 7)) {
			char *p = strchr(buf+7, '\n');
			if(!p) errx(EXIT_FAILURE, "malformed file %s", svn_version_path);
			*p = 0;
			p = buf+7;
			if(*p)
				connection->commit_author = strdup(p);
		} else if(!strncmp(buf, "log=", 4)) {
			/* log entry may span multiple lines, therefore is last in file */
			char *p = strchr(buf+4, '\n');
			if(p) *p = 0;
			connection->commit_msg = malloc(strlen(buf+4)+1);
			memcpy(connection->commit_msg, buf+4, strlen(buf+4)+1);
			in_log = 1;
		}
	}
	fclose(f);
}

static void load_known_files(connector *connection) {
	struct stat local;
	int fd;
	char *md5, *value, *path;
	size_t length;
	struct tree_node *data;

	length = strlen(connection->path_work) + MAXNAMLEN;

	connection->known_files_old = (char *)malloc(length);
	connection->known_files_new = (char *)malloc(length);

	snprintf(connection->known_files_old, length, "%s/known_files", connection->path_work);
	snprintf(connection->known_files_new, length, "%s/known_files.new", connection->path_work);

	if (stat(connection->known_files_old, &local) != -1) {
		connection->known_files_size = local.st_size;

		if ((connection->known_files = (char *)malloc(connection->known_files_size + 1)) == NULL)
			err(EXIT_FAILURE, "connection.known_files malloc");

		if ((fd = open(connection->known_files_old, O_RDONLY)) == -1)
			err(EXIT_FAILURE, "open file (%s)", connection->known_files_old);

		if (read(fd, connection->known_files, connection->known_files_size) != connection->known_files_size)
			err(EXIT_FAILURE, "read file error (%s)", connection->known_files_old);

		connection->known_files[connection->known_files_size] = '\0';
		close(fd);
		value = connection->known_files;

		while (*value) {
			md5 = value;
			path = strchr(value, '\t') + 1;
			value = strchr(path, '\n');
			*value++ = '\0';
			md5[32] = '\0';
			data = (struct tree_node *)malloc(sizeof(struct tree_node));
			data->path = strdup(path);
			data->md5 = strdup(md5);
			RB_INSERT(tree_known_files, &known_files, data);
		}
	}
}

/*
 * main
 *
 * A lightweight, dependency-free program to pull source from an Apache Subversion server.
 */

int
main(int argc, char **argv)
{
	connector connection = {
		.response_blocks = 16,
		.verbosity = 1,
		.family = AF_UNSPEC,
		.protocol = HTTPS,
		.socket_descriptor = -1,
	};

	struct tree_node  *data, *found, *next;
	file_node        **file;

	char   command[COMMAND_BUFFER + 1], *end;
	char  *md5, *path, *start, temp_buffer[BUFFER_UNIT], *value;
	char   svn_version_path[255];
	int    b;
	int    c, command_count;
	int    f, f0, fd, file_count, file_max, length;

	file = NULL;

	file_count = command_count = 0;
	f = f0 = length = 0;

	file_max = BUFFER_UNIT;

	if ((file = (file_node **)malloc(file_max * sizeof(file_node **))) == NULL)
		err(EXIT_FAILURE, "process_directory source malloc");

	command[0] = '\0';

	getopts_svn(argc, argv, &connection);

	/* Create the destination directories if they doesn't exist. */

	if(connection.path_target) create_directory(connection.path_target);
	if(connection.path_work) {
		create_directory(connection.path_work);
		snprintf(svn_version_path, sizeof(svn_version_path),
			"%s/revision", connection.path_work);
	} else svn_version_path[0] = 0;

	if(connection.protocol == NONE) {
		read_revision_file(&connection, svn_version_path);
		write_info_or_log(&connection);
		return 0;
	}


	/* Load the list of known files and MD5 signatures, if they exist. */

	if(connection.path_work) {
		load_known_files(&connection);

		if ((connection.extra_files) || (connection.trim_tree))
			find_local_files_and_directories(connection.path_target, "", 1);
		else
			find_local_files_and_directories(connection.path_target, "", 0);
	}

	/* Initialize connection with the server and get the latest revision number. */

	if ((connection.response = (char *)malloc(connection.response_blocks * BUFFER_UNIT + 1)) == NULL)
		err(EXIT_FAILURE, "main connection.response malloc");

	reset_connection(&connection);

	/* Send initial response string. */

	if (connection.protocol == SVN) {
		connection.response_groups = 1;
		process_command_svn(&connection, "", 0);

		snprintf(command,
			COMMAND_BUFFER,
			"( 2 ( edit-pipeline svndiff1 absent-entries commit-revprops depth log-revprops atomic-revprops partial-replay ) %ld:svn://%s/%s %ld:svnup-%s ( ) )\n",
			strlen(connection.address) + strlen(connection.branch) + 7,
			connection.address,
			connection.branch,
			strlen(SVNUP_VERSION) + 6,
			SVNUP_VERSION);

		process_command_svn(&connection, command, 0);

		start = connection.response;
		end = connection.response + connection.response_length;
		if (check_command_success(connection.protocol, &start, &end))
			exit(EXIT_FAILURE);

		/* Login anonymously. */

		connection.response_groups = 2;
		process_command_svn(&connection, "( ANONYMOUS ( 0: ) )\n", 0);

		/* Get latest revision number. */

		if (connection.revision <= 0) {
			process_command_svn(&connection, "( get-latest-rev ( ) )\n", 0);

			start = connection.response;
			end = connection.response + connection.response_length;
			if (check_command_success(connection.protocol, &start, &end))
				exit(EXIT_FAILURE);

			if ((start != NULL) && starts_with_lit(start, "( success ( ")) {
				start += LIT_LEN("( success ( ");
				value = start;
				while (*start != ' ') start++;
				*start = '\0';

				connection.revision = strtol(value, (char **)NULL, 10);
			} else errx(EXIT_FAILURE, "Cannot retrieve latest revision.");
		}

		/* Check to make sure client-supplied remote path is a directory. */

		snprintf(command,
			COMMAND_BUFFER,
			"( check-path ( 0: ( %d ) ) )\n",
			connection.revision);
		process_command_svn(&connection, command, 0);

		if ((strcmp(connection.response, "( success ( ( ) 0: ) )") != 0) &&
		    (strcmp(connection.response + 23, "( success ( dir ) ) ") != 0))
			errx(EXIT_FAILURE,
				"Remote path %s is not a repository directory.\n%s",
				connection.branch,
				connection.response);

		process_log_svn(&connection);
	}

	else if (connection.protocol >= HTTP) {
		char url[512];
		static const char *footer =
			"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
			"<D:options xmlns:D=\"DAV:\">"
			"<D:activity-collection-set></D:activity-collection-set>"
			"</D:options>\r\n";

		snprintf(url, sizeof url, "/%s", connection.branch);
		craft_http_packet(connection.address, url, "OPTIONS", footer, command);
		connection.response_groups = 2;
		process_command_http(&connection, command);

		/* Get the latest revision number. */

		if (connection.revision <= 0) {
			if ((value = strstr(connection.response, "SVN-Youngest-Rev: ")) == NULL)
				errx(EXIT_FAILURE, "Cannot find revision number.");
			else
				connection.revision = strtol(value + 18, (char **)NULL, 10);
		}

		char buf[1024];
		if(!http_extract_header_value(connection.response, "SVN-Repository-Root", buf, sizeof  buf)) {
			errx(EXIT_FAILURE, "Cannot find SVN Repository Root.");
		}
		assert(buf[0] == '/');
		connection.root = strdup(buf + 1 /* skip leading '/' */);
		if ((path = strstr(connection.branch, connection.root))) {
			if(strlen(connection.branch) == strlen(connection.root))
				path = "";
			else
				path += strlen(connection.root) + 1;
		}
		else errx(EXIT_FAILURE, "Cannot find SVN Repository Trunk.");

		connection.trunk = strdup(path);

		if(http_extract_header_value(connection.response, "SVN-Rev-Root-Stub", buf, sizeof  buf)) {
			assert(buf[0] == '/');
			connection.rev_root_stub = strdup(buf);
		}

		if(connection.rev_root_stub) process_log_http(&connection);
	}

	if (connection.job == SVN_LOG || connection.job == SVN_INFO) {
		write_info_or_log(&connection);
		return 0;
	}

	if (connection.verbosity)
		printf("# Revision: %d\n", connection.revision);

	if (connection.verbosity > 1) {
		fprintf(stderr, "# Protocol: %s\n", protocol_to_string(connection.protocol));
		fprintf(stderr, "# Address: %s\n", connection.address);
		fprintf(stderr, "# Port: %d\n", connection.port);
		fprintf(stderr, "# Branch: %s\n", connection.branch);
		fprintf(stderr, "# Target: %s\n", connection.path_target);
		fprintf(stderr, "# Trim tree: %s\n", connection.trim_tree ? "Yes" : "No");
		fprintf(stderr, "# Show extra files: %s\n", connection.extra_files ? "Yes" : "No");
		fprintf(stderr, "# Known files directory: %s\n", connection.path_work);
	}

	/* at this point, we're checking out a revision, so we request report(s) containing
	   the names of all files and dirs in that revision, including some additional
	   properties that vary among protocol and features of the server */

	if (connection.protocol == SVN) {
		connection.response_groups = 2;

		snprintf(command,
			COMMAND_BUFFER,
			"( get-dir ( 0: ( %d ) false true ( kind size ) false ) )\n",
			connection.revision);

		process_report_svn(&connection, command, &file, &file_count, &file_max);
	}

	if (connection.protocol >= HTTP) {
		process_report_http(&connection, &file, &file_count, &file_max);

		start = connection.response;
		end = connection.response + connection.response_length;
		if (check_command_success(connection.protocol, &start, &end))
			exit(EXIT_FAILURE);
	}

	/* if we have received the md5 checksum already, filter out the files that
	   exist locally and have a matching checksum, so we don't need to download them,
	   nor request additional properties about them. */
	for (f = 0; f < file_count; ++f) {
		check_md5(&connection, file[f]);
	}

	/* Get additional file information not contained in the first report and store the
	   commands in a list. */

	stringlist *buffered_commands = stringlist_new(32);

	/* only retrieve additional information about files
	   if we haven't received inline props already */
	if (!connection.inline_props)
	for (f = 0; f < file_count; f++) {
		temp_buffer[0] = '\0';

		if (connection.protocol == SVN)
			snprintf(temp_buffer,
				BUFFER_UNIT,
				"( get-file ( %zd:%s ( %d ) true false false ) )\n",
				strlen(file[f]->path),
				file[f]->path,
				connection.revision);

		if (connection.protocol >= HTTP) {
			if (file[f]->download) {
				snprintf(temp_buffer,
					BUFFER_UNIT,
					"PROPFIND %s HTTP/1.1\r\n"
					"Depth: 1\r\n"
					"Host: %s\r\n\r\n",
					file[f]->href,
					connection.address);
			}
		}

		if (temp_buffer[0] != '\0') {
			stringlist_add_dup(buffered_commands, temp_buffer);
		}
	}

	/* Process the additional commands to retrieve extended attributes.
	   In case of SVN, this includes the md5 checksum and special(i.e. symlink)
	   and executable properties, for HTTP only the latter 2 plus filesize. */

#define MAX_HTTP_REQUESTS_PER_PACKET 95

	char *chain;
	size_t chain_count = connection.protocol >= HTTP ? MAX_HTTP_REQUESTS_PER_PACKET : 0;
	f = f0 = 0;
	while ((chain = concat_stringlist(buffered_commands, BUFFER_UNIT, &chain_count))) {
		size_t chain_items = chain_count;
		chain_count = connection.protocol >= HTTP ? MAX_HTTP_REQUESTS_PER_PACKET : 0;
		connection.response_groups = chain_items * 2;

		if (connection.protocol >= HTTP)
			process_command_http(&connection, chain);

		if (connection.protocol == SVN)
			process_command_svn(&connection, chain, 0);

		free(chain);

		start = connection.response;
		end = start + connection.response_length;

		command[0] = '\0';
		connection.response_groups = 0;

		for (length = 0, c = 0; c < chain_items; c++) {
			if (connection.protocol >= HTTP)
			while (f < file_count && file[f]->download == 0) {
				/* on http, skip files that already had their md5 checked,
				   therefore no PROPFIND request was submitted,
				   so they're not in the chain */
				if (connection.verbosity > 1)
					progress_indicator(&connection, file[f]->path, f, file_count);

				f++;
			}

			if (check_command_success(connection.protocol, &start, &end))
				exit(EXIT_FAILURE);

			if (connection.protocol >= HTTP)
				parse_response_group(&connection, &start, &end);

			if (connection.protocol == SVN)
				end = strchr(start, '\0');

			parse_additional_attributes(&connection, start, end, file[f]);

			if (connection.verbosity > 1)
				progress_indicator(&connection, file[f]->path, f, file_count);

			start = end + 1;
			f++;
		}
	}
	stringlist_free(buffered_commands);

	/* check md5 again for those still unchecked; in case we only retrieved
	   the checked-in file's checksum right now via additional attributes. */
	for (f = 0; f < file_count; ++f) {
		check_md5(&connection, file[f]);
	}

	buffered_commands = stringlist_new(64);

	for (f=0; f < file_count; ++f) {
		if (file[f]->download) {
			if (connection.protocol >= HTTP)
				snprintf(temp_buffer,
					BUFFER_UNIT,
					"GET %s HTTP/1.1\r\n"
					"Host: %s\r\n"
					"Connection: Keep-Alive\r\n\r\n",
					file[f]->href,
					connection.address);

			if (connection.protocol == SVN)
				snprintf(temp_buffer,
					BUFFER_UNIT,
					"( get-file ( %zd:%s ( %d ) false true false ) )\n",
					strlen(file[f]->path),
					file[f]->path,
					connection.revision);

			stringlist_add_dup(buffered_commands, temp_buffer);
		}
	}

	/* download the actual files missing from tree */
	chain_count = connection.protocol >= HTTP ? MAX_HTTP_REQUESTS_PER_PACKET : 0;
	f = f0 = 0;
	while ((chain = concat_stringlist(buffered_commands, BUFFER_UNIT, &chain_count))) {
		size_t chain_items = chain_count;
		size_t file_incs = 0;
		chain_count = connection.protocol >= HTTP ? MAX_HTTP_REQUESTS_PER_PACKET : 0;
		connection.response_groups = chain_items * 2;

		while (f < file_count && file_incs < chain_items) {
			if(file[f]->download != 0) ++file_incs;
			++f;
		}
		get_files(&connection, chain, connection.path_target,
				file, f0, f - 1);

		if ((connection.verbosity > 1) && (f < file_count))
			progress_indicator(&connection, file[f]->path, f, file_count);

		f0 = f;
		free(chain);
	}
	stringlist_free(buffered_commands);

	save_known_file_list(&connection, file, file_count);

	/* Save details about the current revision */
	save_revision_file(&connection, svn_version_path);

	/* Any files left in the tree are safe to delete. */

	for (data = RB_MIN(tree_known_files, &known_files); data != NULL; data = next) {
		next = RB_NEXT(tree_known_files, head, data);

		if ((found = RB_FIND(tree_local_files, &local_files, data)) != NULL)
			tree_node_free(RB_REMOVE(tree_local_files, &local_files, found));

		if (strncmp(connection.path_work, data->path, strlen(connection.path_work)))
			prune(&connection, data->path);

		tree_node_free(RB_REMOVE(tree_known_files, &known_files, data));
	}

	if (connection.verbosity > 1)
		printf("\r\e[0K\r");

	/* Print/prune any local files left. */

	for (data = RB_MIN(tree_local_files, &local_files); data != NULL; data = next) {
		next = RB_NEXT(tree_local_files, head, data);

		if (connection.trim_tree) {
			/* exempt .git/ from being removed, as it may be used by svn2git tool */
			if(!strncmp(data->path, "/.git/", 6)) goto no_prune;

			char buf[1024];
			snprintf(buf, sizeof buf, "%s%s", connection.path_target, data->path);
			if (strncmp(connection.path_work, buf, strlen(connection.path_work)))
				prune(&connection, data->path);
		} else {
			if (connection.extra_files)
				fprintf(stderr, " * %s%s\n", connection.path_target, data->path);
		}

	no_prune:;
		tree_node_free(RB_REMOVE(tree_local_files, &local_files, data));
	}

	/* Prune any empty local directories not found in the repository. */

	if (connection.verbosity > 1)
		fprintf(stderr, "\e[0K\r");

	for (data = RB_MAX(tree_local_directories, &local_directories); data != NULL; data = next) {
		next = RB_PREV(tree_local_directories, head, data);

		char buf[1024];
		snprintf(buf, sizeof buf, "%s/.git/", connection.path_target);

		if (strncmp(data->path, buf, strlen(buf)) && rmdir(data->path) == 0)
			fprintf(stderr, " = %s\n", data->path);

		tree_node_free(RB_REMOVE(tree_local_directories, &local_directories, data));
	}

	/* Wrap it all up. */

	if (close(connection.socket_descriptor) != 0)
		if (errno != EBADF)
			err(EXIT_FAILURE, "close connection failed");

	remove(connection.known_files_old);

	if ((rename(connection.known_files_new, connection.known_files_old)) != 0)
		err(EXIT_FAILURE, "Cannot rename %s", connection.known_files_old);

	if (connection.address)
		free(connection.address);

	if (connection.root)
		free(connection.root);

	if (connection.trunk)
		free(connection.trunk);

	if (connection.branch)
		free(connection.branch);

	if (connection.known_files)
		free(connection.known_files);

	if (connection.path_target)
		free(connection.path_target);

	if (connection.path_work)
		free(connection.path_work);

	if (connection.ssl) {
		SSL_shutdown(connection.ssl);
		SSL_CTX_free(connection.ctx);
		SSL_free(connection.ssl);
	}

	free(connection.commit_author);
	free(connection.commit_msg);
	free(connection.commit_date);

	free(connection.known_files_old);
	free(connection.known_files_new);
	free(connection.response);
	free(file);

	return (0);
}


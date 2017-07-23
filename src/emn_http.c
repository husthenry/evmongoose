#include "emn_http.h"
#include "emn.h"
#include <errno.h>

/* HTTP message */
struct http_message {
	http_parser parser;

	struct emn_str url;
	struct emn_str path;
	struct emn_str query;

	/* Headers */
	struct emn_str header_names[EMN_MAX_HTTP_HEADERS];
	struct emn_str header_values[EMN_MAX_HTTP_HEADERS];

	/* Message body */
	struct emn_str body; /* Zero-length for requests with no body */
};

static void emn_send_http_file(struct emn_client *cli)
{
	struct emn_server *srv = emn_client_get_server(cli);
	struct http_opts *opts = (struct http_opts *)emn_server_get_opts(srv);
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	const char *document_root = opts ? (opts->document_root ? opts->document_root : ".") : ".";
	char *local_path = calloc(1, strlen(document_root) + hm->path.len + 1);
	int code = 200;
	int send_fd = -1;
	struct ebuf *rbuf = emn_get_rbuf(cli);
	
	strcpy(local_path, document_root);
	memcpy(local_path + strlen(document_root), hm->path.p, hm->path.len);

	send_fd = open(local_path, O_RDONLY);
	if (send_fd < 0) {
		switch (errno) {
		case EACCES:
			code = 403;
	        break;
		case ENOENT:
	        code = 404;
	        break;
	      default:
	        code = 500;
		}
	}

	if (send_fd > 0) {
		struct stat st;
		
		fstat(send_fd, &st);

		if (S_ISDIR(st.st_mode)) {
			close (send_fd);
			code = 404;
			goto end;
		}

		emn_send_http_response_line(cli, code, NULL);
    	emn_printf(cli,
              "Content-Type: text/html\r\n"
              "Content-Length: %zu\r\n"
              "Connection: %s\r\n"
              "\r\n",
              st.st_size, http_should_keep_alive(&hm->parser) ? "keep-alive" : "close"
              );
		emn_client_set_send_fd(cli, send_fd);
	}

	if (code != 200)
		emn_send_http_error(cli, code, NULL);

	ebuf_remove(rbuf, rbuf->len);
	
end:
	free(local_path);
}

static void emn_serve_http(struct emn_client *cli)
{
	emn_send_http_file(cli);
}

static int on_url(http_parser *parser, const char *at, size_t len)
{	
	struct emn_client *cli = (struct emn_client *)parser->data;
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	struct http_parser_url url;

	emn_str_init(&hm->url, at, len);
	
	if (http_parser_parse_url(at, len, 0, &url))
		printf("http_parser_parse_url failed\n");
	else {
		if (url.field_set & (1 << UF_PATH))
			emn_str_init(&hm->path, at + url.field_data[UF_PATH].off, url.field_data[UF_PATH].len);

		if (url.field_set & (1 << UF_QUERY))
			emn_str_init(&hm->query, at + url.field_data[UF_QUERY].off, url.field_data[UF_QUERY].len);
	}
		
    return 0;
}

static int on_header_field(http_parser *parser, const char *at, size_t len)
{
	int i = 0;
	struct emn_client *cli = (struct emn_client *)parser->data;
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	struct emn_str *header_names = hm->header_names;
	
	while (i < EMN_MAX_HTTP_HEADERS) {
		if (header_names[i].len == 0) {
			emn_str_init(header_names + i, at, len);
			break;
		}
		i++;
	}
	
    return 0;
}

static int on_header_value(http_parser *parser, const char *at, size_t len)
{
	int i = 0;
	struct emn_client *cli = (struct emn_client *)parser->data;
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	struct emn_str *header_values = hm->header_values;
	
	while (i < EMN_MAX_HTTP_HEADERS) {
		if (header_values[i].len == 0) {
			emn_str_init(header_values + i, at, len);
			break;
		}
		i++;
	}
    return 0;
}

static int on_body(http_parser *parser, const char *at, size_t len)
{
	struct emn_client *cli = (struct emn_client *)parser->data;
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	emn_str_init(&hm->body, at, len);
    return 0;
}

static int on_message_complete(http_parser *parser)
{
	struct emn_client *cli = (struct emn_client *)parser->data;
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
#if 0
	int i = 0;
	printf("\n***MESSAGE COMPLETE***\n\n");
	printf("method: %s\n", http_method_str(parser->method));
	printf("uri: %.*s\n", (int)hm->uri.len, hm->uri.p);
	printf("proto: %d.%d\n", parser->http_major, parser->http_minor);

	while (i < EMN_MAX_HTTP_HEADERS) {
		if (hm->header_names[i].len > 0 && hm->header_values[i].len > 0) {
			printf("%.*s: %.*s\n", (int)hm->header_names[i].len, hm->header_names[i].p, 
				(int)hm->header_values[i].len, hm->header_values[i].p);
			i++;
		} else {
			break;
		}
	}

	printf("body:%.*s\n", (int)hm->body.len, hm->body.p);
#endif

	if (!http_should_keep_alive(&hm->parser))
		emn_client_set_flags(cli, EMN_FLAGS_SEND_AND_CLOSE);
		
	if (!emn_call(cli, NULL, EMN_EV_HTTP_REQUEST, hm))
		emn_serve_http(cli);
	
	return 0;
}

static http_parser_settings parser_settings = {
	.on_url              = on_url,
	.on_header_field     = on_header_field,
	.on_header_value     = on_header_value,
	.on_body             = on_body,
	.on_message_complete = on_message_complete
};

static int emn_http_handler(struct emn_client *cli, int event, void *data)
{
	if (event == EMN_EV_ACCEPT) {
		struct http_message *hm = calloc(1, sizeof(struct http_message));
		http_parser_init(&hm->parser, HTTP_REQUEST);
		hm->parser.data = cli;
		emn_client_set_data(cli, (void *)hm);
	}

	emn_call(cli, emn_client_get_handler(cli), event, data);

	if (event == EMN_EV_RECV) {
		size_t nparsed;
		struct ebuf *rbuf = emn_get_rbuf(cli);
		struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
		
		nparsed = http_parser_execute(&hm->parser, &parser_settings, rbuf->buf, rbuf->len);
		printf("nparsed = %zu\n", nparsed);
	}
	
	return 0;
}

inline enum http_method emn_get_http_method(struct emn_client *cli)
{
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	return hm->parser.method;
}

inline struct emn_str *emn_get_http_url(struct emn_client *cli)
{
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	return &hm->url;
}

inline struct emn_str *emn_get_http_path(struct emn_client *cli)
{
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	return &hm->path;
}

inline struct emn_str *emn_get_http_query(struct emn_client *cli)
{
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	return &hm->query;
}

inline uint8_t emn_get_http_version_major(struct emn_client *cli)
{
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	return hm->parser.http_major;
}

inline uint8_t emn_get_http_version_minor(struct emn_client *cli)
{
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	return hm->parser.http_minor;
}

inline struct emn_str *emn_get_http_header(struct emn_client *cli, const char *name)
{
	int i = 0;
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	struct emn_str *header_names = hm->header_names;
	struct emn_str *header_values = hm->header_values;
	while (i < EMN_MAX_HTTP_HEADERS) {
		if (!strncasecmp(header_names[i].p, name, header_names[i].len)) {
			return header_values + i;
			break;
		}
		i++;
	}
	return NULL;
}

inline struct emn_str *emn_get_http_body(struct emn_client *cli)
{
	struct http_message *hm = (struct http_message *)emn_client_get_data(cli);
	return &hm->body;
}

static const char *emn_http_status_message(int code)
{
	switch (code) {
	case 301:
		return "Moved";
	case 302:
		return "Found";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 416:
		return "Requested Range Not Satisfiable";
	case 418:
		return "I'm a teapot";
	case 500:
		return "Internal Server Error";
	case 502:
		return "Bad Gateway";
	case 503:
		return "Service Unavailable";
	default:
		return "OK";
	}
}

void emn_send_http_response_line(struct emn_client *cli, int code, const char *extra_headers)
{
	emn_printf(cli, "HTTP/1.1 %d %s\r\nServer: Emn %d.%d\r\n", code, emn_http_status_message(code), 
		EMN_VERSION_MAJOR, EMN_VERSION_MINOR);
	if (extra_headers)
		emn_printf(cli, "%s\r\n", extra_headers);
}

void emn_send_http_head(struct emn_client *cli, int code, ssize_t content_length, const char *extra_headers)
{
	emn_send_http_response_line(cli, code, extra_headers);
	if (content_length < 0)
		emn_printf(cli, "%s", "Transfer-Encoding: chunked\r\n");
	else
		emn_printf(cli, "Content-Length: %zd\r\n", content_length);
	emn_send(cli, "\r\n", 2);
}

void emn_send_http_error(struct emn_client *cli, int code, const char *reason)
{
	if (!reason)
		reason = emn_http_status_message(code);

	emn_send_http_head(cli, code, strlen(reason), "Content-Type: text/plain\r\nConnection: close");
	emn_send(cli, reason, strlen(reason));
	emn_client_set_flags(cli, EMN_FLAGS_SEND_AND_CLOSE);
}

void emn_set_protocol_http(struct emn_server *srv, struct http_opts *opts)
{
	emn_sever_set_proto_handler(srv, emn_http_handler);
	emn_sever_set_flags(srv, EMN_FLAGS_HTTP);
	emn_sever_set_opts(srv, (void *)opts);
}


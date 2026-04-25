#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../include/parse.h"

#define ECHO_PORT 9999
#define RECV_CHUNK 4096
#define MAX_HEADER_SIZE 8192
#define MAX_BUFFER_SIZE (1024 * 1024)

#define RESPONSE_400 "HTTP/1.1 400 Bad request\r\n\r\n"
#define RESPONSE_404 "HTTP/1.1 404 Not Found\r\n\r\n"
#define RESPONSE_501 "HTTP/1.1 501 Not Implemented\r\n\r\n"
#define RESPONSE_505 "HTTP/1.1 505 HTTP Version not supported\r\n\r\n"

static int server_sock = -1;

static int close_socket(int fd) {
    if (fd >= 0 && close(fd)) {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

static void handle_signal(const int sig) {
    if (server_sock != -1) {
        fprintf(stderr, "\nReceived signal %d. Closing socket.\n", sig);
        close_socket(server_sock);
    }
    exit(0);
}

static void handle_sigpipe(const int sig) {
    (void)sig;
}

static void ensure_log_dir(void) {
    mkdir("logs", 0755);
}

static void make_time_str(char *out, size_t out_sz) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(out, out_sz, "%a %b %d %H:%M:%S.%Y", &tm_now);
}

static void log_error(const char *fmt, ...) {
    ensure_log_dir();
    FILE *fp = fopen("logs/error.log", "a");
    if (!fp) {
        return;
    }

    char ts[64] = {0};
    make_time_str(ts, sizeof(ts));

    fprintf(fp, "[%s] [error] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static void log_access(const char *client_ip,
                       const char *request_line,
                       int status,
                       size_t response_bytes) {
    ensure_log_dir();
    FILE *fp = fopen("logs/access.log", "a");
    if (!fp) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    char ts[64] = {0};
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%d/%b/%Y:%H:%M:%S %z", &tm_now);

    fprintf(fp, "%s - - [%s] \"%s\" %d %zu\n",
            client_ip,
            ts,
            request_line,
            status,
            response_bytes);
    fclose(fp);
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static const char *mime_type_for_path(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "text/html";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(ext, ".txt") == 0) {
        return "text/plain";
    }
    return "application/octet-stream";
}

typedef struct {
    Request *parsed;
    const char *raw;
    int content_length;
    bool connection_close;
    size_t header_len;
    size_t total_len;
} HttpRequest;

static void free_parsed_request(Request *req) {
    if (!req) {
        return;
    }
    free(req->headers);
    free(req);
}

static int parse_int_strict(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 1024 * 1024 * 32) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static const char *find_crlfcrlf(const char *buf, size_t n) {
    if (n < 4) {
        return NULL;
    }
    for (size_t i = 0; i + 3 < n; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

static int parse_one_request(const char *buf, size_t n, HttpRequest *req, int *status_on_error, int sockfd) {
    memset(req, 0, sizeof(*req));
    req->raw = buf;

    const char *header_end_ptr = find_crlfcrlf(buf, n);
    if (!header_end_ptr) {
        if (n > MAX_HEADER_SIZE) {
            *status_on_error = 400;
            return -1;
        }
        return 0;
    }

    req->header_len = (size_t)(header_end_ptr - buf) + 4;
    if (req->header_len > MAX_HEADER_SIZE) {
        *status_on_error = 400;
        return -1;
    }

    req->parsed = parse(buf, (int)req->header_len, sockfd);
    if (!req->parsed) {
        *status_on_error = 400;
        return -1;
    }

    if (strcmp(req->parsed->http_version, "HTTP/1.1") != 0) {
        *status_on_error = 505;
        return -1;
    }

    for (int i = 0; i < req->parsed->header_count; i++) {
        const char *name = req->parsed->headers[i].header_name;
        const char *value = req->parsed->headers[i].header_value;

        if (strcasecmp(name, "Content-Length") == 0) {
            if (parse_int_strict(value, &req->content_length) != 0) {
                *status_on_error = 400;
                return -1;
            }
        } else if (strcasecmp(name, "Connection") == 0) {
            if (strncasecmp(value, "close", 5) == 0) {
                req->connection_close = true;
            }
        }
    }

    req->total_len = req->header_len + (size_t)req->content_length;
    if (req->total_len > n) {
        return 0;
    }

    return 1;
}

static size_t send_simple_response(int fd, const char *resp) {
    size_t n = strlen(resp);
    if (send_all(fd, resp, n) != 0) {
        return 0;
    }
    return n;
}

static bool map_uri_to_path(const char *uri, char *path, size_t path_sz) {
    if (!uri || uri[0] != '/' || strstr(uri, "..") != NULL || strchr(uri, '\\') != NULL) {
        return false;
    }

    char clean_uri[4096] = {0};
    size_t i = 0;
    while (uri[i] && uri[i] != '?' && uri[i] != '#' && i < sizeof(clean_uri) - 1) {
        clean_uri[i] = uri[i];
        i++;
    }
    clean_uri[i] = '\0';

    if (strcmp(clean_uri, "/") == 0) {
        snprintf(path, path_sz, "static_site/index.html");
        return true;
    }

    snprintf(path, path_sz, "static_site/%s", clean_uri + 1);
    return true;
}

static size_t handle_get_head(int fd, const HttpRequest *req, int *status_out) {
    char path[4096] = {0};
    if (!map_uri_to_path(req->parsed->http_uri, path, sizeof(path))) {
        *status_out = 404;
        return send_simple_response(fd, RESPONSE_404);
    }

    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        *status_out = 404;
        return send_simple_response(fd, RESPONSE_404);
    }

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        log_error("open(%s) failed: %s", path, strerror(errno));
        *status_out = 404;
        return send_simple_response(fd, RESPONSE_404);
    }

    const char *mime = mime_type_for_path(path);
    char header[512] = {0};
    int hlen = snprintf(header,
                        sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: %ld\r\n"
                        "Content-Type: %s\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n",
                        (long)st.st_size,
                        mime);

    if (hlen <= 0 || send_all(fd, header, (size_t)hlen) != 0) {
        close(file_fd);
        *status_out = 400;
        return 0;
    }

    size_t sent = (size_t)hlen;

    if (strcmp(req->parsed->http_method, "HEAD") == 0) {
        close(file_fd);
        *status_out = 200;
        return sent;
    }

    char io_buf[4096];
    while (1) {
        ssize_t n = read(file_fd, io_buf, sizeof(io_buf));
        if (n < 0) {
            log_error("read(%s) failed: %s", path, strerror(errno));
            close(file_fd);
            *status_out = 400;
            return sent;
        }
        if (n == 0) {
            break;
        }
        if (send_all(fd, io_buf, (size_t)n) != 0) {
            close(file_fd);
            *status_out = 400;
            return sent;
        }
        sent += (size_t)n;
    }

    close(file_fd);
    *status_out = 200;
    return sent;
}

static size_t handle_post_echo(int fd, const HttpRequest *req, int *status_out) {
    char header[256] = {0};
    int hlen = snprintf(header,
                        sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n",
                        req->total_len);
    if (hlen <= 0) {
        *status_out = 400;
        return 0;
    }

    if (send_all(fd, header, (size_t)hlen) != 0 || send_all(fd, req->raw, req->total_len) != 0) {
        *status_out = 400;
        return 0;
    }

    *status_out = 200;
    return (size_t)hlen + req->total_len;
}

static size_t dispatch_request(int fd, const HttpRequest *req, int *status_out) {
    if (strcmp(req->parsed->http_method, "GET") == 0 || strcmp(req->parsed->http_method, "HEAD") == 0) {
        return handle_get_head(fd, req, status_out);
    }
    if (strcmp(req->parsed->http_method, "POST") == 0) {
        return handle_post_echo(fd, req, status_out);
    }
    *status_out = 501;
    return send_simple_response(fd, RESPONSE_501);
}

int main(void) {
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGSEGV, handle_signal);
    signal(SIGABRT, handle_signal);
    signal(SIGQUIT, handle_signal);
    signal(SIGTSTP, handle_signal);
    signal(SIGFPE, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGPIPE, handle_sigpipe);

    socklen_t cli_size;
    struct sockaddr_in addr, cli_addr;
    fprintf(stdout, "----- Echo Server -----\n");

    if ((server_sock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Failed creating socket.\n");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        fprintf(stderr, "Failed setting socket options.\n");
        return EXIT_FAILURE;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr))) {
        close_socket(server_sock);
        fprintf(stderr, "Failed binding socket.\n");
        return EXIT_FAILURE;
    }

    if (listen(server_sock, 5)) {
        close_socket(server_sock);
        fprintf(stderr, "Error listening on socket.\n");
        return EXIT_FAILURE;
    }

    while (1) {
        cli_size = sizeof(cli_addr);
        fprintf(stdout, "\nWaiting for connection...\n");
        int client_sock = accept(server_sock, (struct sockaddr *)&cli_addr, &cli_size);

        if (client_sock == -1) {
            fprintf(stderr, "Error accepting connection.\n");
            continue;
        }

        char client_ip[64] = {0};
        snprintf(client_ip, sizeof(client_ip), "%s", inet_ntoa(cli_addr.sin_addr));
        fprintf(stdout, "New connection from %s:%d\n", client_ip, ntohs(cli_addr.sin_port));

        char *conn_buf = (char *)malloc(RECV_CHUNK);
        if (!conn_buf) {
            close(client_sock);
            continue;
        }
        size_t conn_cap = RECV_CHUNK;
        size_t conn_len = 0;
        bool done = false;

        while (!done) {
            if (conn_cap - conn_len < RECV_CHUNK) {
                size_t new_cap = conn_cap * 2;
                if (new_cap > MAX_BUFFER_SIZE) {
                    send_simple_response(client_sock, RESPONSE_400);
                    log_error("client buffer exceeds %d bytes", MAX_BUFFER_SIZE);
                    break;
                }
                char *new_buf = (char *)realloc(conn_buf, new_cap);
                if (!new_buf) {
                    send_simple_response(client_sock, RESPONSE_400);
                    break;
                }
                conn_buf = new_buf;
                conn_cap = new_cap;
            }

            ssize_t n = recv(client_sock, conn_buf + conn_len, conn_cap - conn_len, 0);
            if (n <= 0) {
                break;
            }
            conn_len += (size_t)n;

            while (1) {
                HttpRequest req;
                int err_status = 0;
                int parse_res = parse_one_request(conn_buf, conn_len, &req, &err_status, client_sock);
                if (parse_res == 0) {
                    break;
                }

                if (parse_res < 0) {
                    const char *resp = RESPONSE_400;
                    if (err_status == 501) {
                        resp = RESPONSE_501;
                    } else if (err_status == 505) {
                        resp = RESPONSE_505;
                    }
                    size_t rb = send_simple_response(client_sock, resp);
                    log_access(client_ip, "BAD_REQUEST", err_status == 0 ? 400 : err_status, rb);
                    free_parsed_request(req.parsed);
                    done = true;
                    break;
                }

                int status = 0;
                size_t rb = dispatch_request(client_sock, &req, &status);
                char reqline[2200] = {0};
                snprintf(reqline,
                         sizeof(reqline),
                         "%.49s %.2047s %.49s",
                         req.parsed->http_method,
                         req.parsed->http_uri,
                         req.parsed->http_version);
                log_access(client_ip, reqline, status, rb);

                bool close_conn = req.connection_close;
                size_t consumed = req.total_len;
                free_parsed_request(req.parsed);
                memmove(conn_buf, conn_buf + consumed, conn_len - consumed);
                conn_len -= consumed;

                if (close_conn) {
                    done = true;
                    break;
                }
            }
        }

        free(conn_buf);
        close(client_sock);
        fprintf(stdout, "Closed connection\n");
    }

    close_socket(server_sock);
    return EXIT_SUCCESS;
}

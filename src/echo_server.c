#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include "../include/parse.h"

#define ECHO_PORT 9999
#define BUF_SIZE 4096

int sock = -1, client_sock = -1;
char buf[BUF_SIZE];

int close_socket(int sock) {
    if (close(sock)) {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

void handle_signal(const int sig) {
    if (sock != -1) {
        fprintf(stderr, "\nReceived signal %d. Closing socket.\n", sig);
        close_socket(sock);
    }
    exit(0);
}

void handle_sigpipe(const int sig) {
    return;
}

int main(int argc, char *argv[]) {
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

    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Failed creating socket.\n");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        fprintf(stderr, "Failed setting socket options.\n");
        return EXIT_FAILURE;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
        close_socket(sock);
        fprintf(stderr, "Failed binding socket.\n");
        return EXIT_FAILURE;
    }

    if (listen(sock, 5)) {
        close_socket(sock);
        fprintf(stderr, "Error listening on socket.\n");
        return EXIT_FAILURE;
    }

    while (1) {
        cli_size = sizeof(cli_addr);
        fprintf(stdout, "\nWaiting for connection...\n");
        client_sock = accept(sock, (struct sockaddr *)&cli_addr, &cli_size);

        if (client_sock == -1) {
            fprintf(stderr, "Error accepting connection.\n");
            continue;
        }

        fprintf(stdout, "New connection from %s:%d\n",
                inet_ntoa(cli_addr.sin_addr),
                ntohs(cli_addr.sin_port));

        memset(buf, 0, BUF_SIZE);
        int readret = recv(client_sock, buf, BUF_SIZE - 1, 0);

        if (readret <= 0) {
            close(client_sock);
            continue;
        }

        // HTTP 解析
        Request *req = parse(buf, readret, client_sock);

        if (req == NULL) {
            // 400 错误
            const char *resp_400 = "HTTP/1.1 400 Bad Request\r\n\r\n";
            send(client_sock, resp_400, strlen(resp_400), 0);
            fprintf(stdout, "→ 返回 400 Bad Request\n");
        }
        else if (strcmp(req->http_method, "GET") == 0 ||
                 strcmp(req->http_method, "HEAD") == 0 ||
                 strcmp(req->http_method, "POST") == 0)
        {

            // 1. 构造合法的 HTTP 200 响应头
            const char *header = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n";
            send(client_sock, header, strlen(header), 0);

            // 正常返回
            send(client_sock, buf, readret, 0);
            fprintf(stdout, "→ 支持方法：%s\n", req->http_method);
        }
        else {
            // 501 不支持
            const char *resp_501 = "HTTP/1.1 501 Not Implemented\r\n\r\n";
            send(client_sock, resp_501, strlen(resp_501), 0);
            fprintf(stdout, "→ 返回 501 Not Implemented\n");
        }

        // ======================
        // 关键：删掉了 free_request！
        // ======================

        close(client_sock);
        fprintf(stdout, "Closed connection\n");
    }

    close_socket(sock);
    return EXIT_SUCCESS;
}
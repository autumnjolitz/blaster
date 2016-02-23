#include <libmill.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <contrib/http_parser.h>

#ifdef DEBUG
#define DEBUG_PRINTF(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( false )
#else
#define DEBUG_PRINTF(...) do{ } while ( false )
#endif

typedef struct REQUEST {
    char *path; //4 or 8 bytes
    int *path_length; // 4 or 8 bytes
    struct http_parser_url *url_parser; // 4 or 8 bytes
    bool *keep_alive; // 1 bytes
} REQUEST;

int url_ready(http_parser* parser, const char *url, size_t length) {
    REQUEST* request = (REQUEST* )parser->data;

    http_parser_parse_url(url, length, parser->method == HTTP_CONNECT, request->url_parser);

    uint16_t offset = (request->url_parser)->field_data[UF_PATH].off;
    uint16_t path_length = (request->url_parser)->field_data[UF_PATH].len;

    *request->path_length = length;
    memcpy(request->path, url + offset, path_length);
    return 0;
}

int on_headers_ready(http_parser* parser) {
    REQUEST* request = (REQUEST* )parser->data;
    *(request->keep_alive) = (bool) http_should_keep_alive(parser);
    return 0;
}

char no_keep_alive[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nOk\n";
char keep_alive_capable[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nKeep-Alive: timeout=15, max=200\r\nConnection: keep-alive\r\n\r\nOk\n";


coroutine void handle_request(tcpsock client) {
    char path[2048] = {0};
    char buf[8192];
    struct http_parser_url url_parser = {};
    http_parser_url_init(&url_parser);
    int path_length = -1;
    bool keep_alive_request = false;

    REQUEST request = {path, &path_length, &url_parser, &keep_alive_request};
    int64_t t_s = 0;
    int reqs_left = 199;
    size_t num_bytes;
    size_t num_bytes_parsed;
    http_parser_settings settings;
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);

    parser.data = &request;
    settings.on_url = url_ready;
    settings.on_headers_complete = on_headers_ready;

    begin:
    path_length = -1;

    while(true) {
        int64_t deadline = now() + 5;
        num_bytes = tcprecv(client, buf, sizeof(buf), deadline);
        if (num_bytes == 0) {
            break;
        }
        if (errno != ETIMEDOUT)
        {
            if (errno == ECONNRESET) {
                DEBUG_PRINTF("RESET! reqs left: %i bytes: %zu data: %s\n", reqs_left, num_bytes, buf);
                goto cleanup;
            }
            DEBUG_PRINTF("Errno %d\n", errno);
            goto cleanup;
        }
        t_s = now();
        num_bytes_parsed = http_parser_execute(&parser, &settings, buf, num_bytes);
        if (num_bytes_parsed == 0)
            break;
    }
    num_bytes_parsed = http_parser_execute(&parser, &settings, buf, num_bytes);
    if (path_length > -1) {
        if (keep_alive_request) {
            tcpsend(client, keep_alive_capable, sizeof(keep_alive_capable), -1);
            tcpflush(client, -1);
            if ((now() - t_s) > 15*1000 || reqs_left == 0) {
                goto cleanup;
            }
            reqs_left--;
            goto begin;
        } else {
            tcpsend(client, no_keep_alive, sizeof(no_keep_alive), -1);
            tcpflush(client, -1);
        }
    }


    cleanup:
    tcpclose(client);
}

int main(int arg_count, char* args[]) {
    int port = 5555;
    int num_processes = 1;
    if (arg_count > 1) {
        port = atoi(args[1]);
        if (arg_count > 2) {
            num_processes = atoi(args[2]);
        }
    }
    if (port < 1) {
        perror("Ports cannot be less than 1");
        return 1;
    }
    if (num_processes < 1) {
        perror("Num processes cannot be less than 1");
        return 2;
    }
    ipaddr address = iplocal(NULL, port, 0);
    tcpsock server_socket = tcplisten(address, 100);
    pid_t current_pid = getpid();
    if (num_processes > 1) {
        printf("Starting %d processes\n", num_processes);
        for (int i = 0; i < num_processes - 1; ++i) {
            pid_t pid = mfork();
            if (pid < 0) {
                perror("Cannot fork processes");
                return 4;
            }
            if (pid > 0) {
                current_pid = pid;
                break;
            }
        }
    }

    if (server_socket == NULL) {
        printf("Cannot open listening socket on port %d", port);
        return 3;
    }
    while(true) {
        tcpsock client_tunnel = tcpaccept(server_socket, -1);
        if (client_tunnel == NULL)
            continue;
        go(handle_request(client_tunnel));
    }
    return 0;
}

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

// Let parsing for our request data take no more than below in seconds
#define MAX_REQUEST_LIFETIME_S 10

typedef struct BLASTER_HTTP_REQUEST {
    char *path; //4 or 8 bytes
    int *path_length; // 4 or 8 bytes
    struct http_parser_url *url_parser; // 4 or 8 bytes
    bool *keep_alive; // 4 or 8 bytes
    bool *body_ready; // 4 or 8
} BLASTER_HTTP_REQUEST;

// Handlers for parsing HTTP requests:
int on_url_ready(http_parser* parser, const char *url, size_t length) {
    BLASTER_HTTP_REQUEST* request = (BLASTER_HTTP_REQUEST* )parser->data;
    struct http_parser_url *url_parser = request->url_parser;

    int result = http_parser_parse_url(url, length, parser->method == HTTP_CONNECT, url_parser);
    if (result) {
        DEBUG_PRINTF("Unexpected code %i from http_parser_url!", result);
        return -1;
    }

    uint16_t offset = url_parser->field_data[UF_PATH].off;
    uint16_t path_length = url_parser->field_data[UF_PATH].len;

    *request->path_length = length;
    memcpy(request->path, url + offset, path_length);
    return 0;
}

int on_headers_ready(http_parser* parser) {
    BLASTER_HTTP_REQUEST* request = (BLASTER_HTTP_REQUEST* )parser->data;
    *(request->keep_alive) = (bool) http_should_keep_alive(parser);
    return 0;
}

int on_body_ready(http_parser* parser) {
    BLASTER_HTTP_REQUEST* request = (BLASTER_HTTP_REQUEST* )parser->data;
    *(request->body_ready) = true;
    return 0;
}

// Hardcoded HTTP responses
char no_keep_alive[70] = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nHello World\n";
char keep_alive_capable[132] = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nContent-Type: text/plain\r\nKeep-Alive: timeout=5, max=40\r\nConnection: keep-alive\r\n\r\nHello World\n";


char error_no_path_found[145] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 52\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nInvalid path specifier - malformatted HTTP request?\n";
char error_path_too_long[108] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 15\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nPath too long.\n";
char error_404_not_found[107] = "HTTP/1.1 404 Not Found\r\nContent-Length: 16\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nRoute not found\n";
char empty_response[95] = "HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOkay\n";
/*
** handle_request(tcpsock client)
** This is our request handler. It sets up an HTTP parser, signals various
** boolean pointers to indicate state, defines deadlines and invokes a yield after
** a potentially expensive function (parsing HTTP headers).
** The on_ functions above are used to checkpoint states in parsing and convey data
** back to the suspended coroutine.
**
** Stack allocation is used in conjunction with pointers to elide expensive copying of structs
** and costly malloc()s
*/

#define match_raw_path(client_path, path, path_length, match) \
    (*match = (bool)(path_length == strlen(client_path) && memcmp(path, client_path, path_length) == 0))

coroutine void handle_request(tcpsock client, int requests_left, http_parser_settings *settings) {
    char path[200] = {0};
    int path_length = 0;
    struct http_parser_url url_parser;
    http_parser_url_init(&url_parser);

    bool keep_alive = false;
    bool body_ready = false;

    BLASTER_HTTP_REQUEST request = {path, &path_length, &url_parser, &keep_alive, &body_ready};


    http_parser parser = {.data = &request};
    http_parser_init(&parser, HTTP_REQUEST);

    int64_t last_wakeup = 0;
    int64_t request_start_ts = now();
    ipaddr client_address = tcpaddr(client);
    int64_t end_time_ts = request_start_ts + MAX_REQUEST_LIFETIME_S*1000;

    while(now() < end_time_ts) {
        char buf[2048] = {0};
        size_t num_bytes_read = tcprecv(client, buf, sizeof(buf), now());
        if (errno == ECONNRESET) {
            char client_address_repr[IPADDR_MAXSTRLEN];
            ipaddrstr(client_address, client_address_repr);
            DEBUG_PRINTF("[PID %i] Client %s sent RST, %d requests left\n", getpid(), client_address_repr, requests_left);
            break;
        }
        if(num_bytes_read > 0) {
            last_wakeup = now();
            http_parser_execute(&parser, settings, buf, num_bytes_read);
        } else {
            yield();
        }
        if (now() - last_wakeup >= 5*1000 && keep_alive) {
            char client_address_repr[IPADDR_MAXSTRLEN];
            ipaddrstr(client_address, client_address_repr);
            DEBUG_PRINTF("[PID %i] Client %s idled for more than 5 seconds with %d requests left over. Flushing and closing.\n", getpid(), client_address_repr, requests_left);
            tcpflush(client, -1);
            break;
        }
        if(body_ready) {
            break;
        }
    }
    bool matched = false;
    if (body_ready) {
        char* response = error_no_path_found;
        size_t response_length = sizeof(error_no_path_found);
        if (path_length > 0) {
            if (path_length > 199) {
                response = error_path_too_long;
                response_length = sizeof(error_path_too_long);
            } else {
                if (match_raw_path("/", path, path_length, &matched)) {
                    response = no_keep_alive;
                    response_length = sizeof(no_keep_alive);
                    if(keep_alive) {
                        response = keep_alive_capable;
                        response_length = sizeof(keep_alive_capable);
                    }
                } else if (match_raw_path("/goredump", path, path_length, &matched)) {
                    goredump();
                    response = empty_response;
                    response_length = sizeof(empty_response);
                } else {
                    response = error_404_not_found;
                    response_length = sizeof(error_404_not_found);
                }
                
            }
        }
        tcpsend(client, response, response_length, -1);
        tcpflush(client, -1);
        if (matched) {
            if (keep_alive && requests_left > 0)
                goto reuse;
        }
    }

    tcpclose(client);
    return;
    reuse:
        handle_request(client, requests_left - 1, settings);
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
    tcpsock server_socket = tcplisten(address, 10);
    pid_t current_pid = getpid();
    printf("Starting %d process(es)\n", num_processes);
    if (num_processes > 1) {
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
    printf("[%i] Listening on port %d\n", current_pid, port);
    // Since we're always parsing things the same way, let's share the stack alloc'ed
    // settings
    http_parser_settings settings;
    http_parser_settings_init(&settings);
    settings.on_url = on_url_ready;
    settings.on_headers_complete = on_headers_ready;
    settings.on_message_complete = on_body_ready;

    // Event loop
    // Any time our server_socket is ready, check if the client socket is good
    // the launch a fiber to deal with it.
    while(true) {
        int64_t deadline = now() + 10;
        tcpsock client_tunnel = tcpaccept(server_socket, deadline);
        if (client_tunnel == NULL) {
            continue;
        }
        go(handle_request(client_tunnel, 40, &settings));
    }
    return 0;
}

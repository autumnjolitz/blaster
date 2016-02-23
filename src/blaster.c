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

    http_parser_parse_url(url, length, parser->method == HTTP_CONNECT, url_parser);

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
char no_keep_alive[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nOk\n";
char keep_alive_capable[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nKeep-Alive: timeout=5, max=20\r\nConnection: keep-alive\r\n\r\nOk\n";

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
coroutine void handle_request(tcpsock client, int requests_left) {
    if (requests_left == 0) {
        goto close;
    }
    char path[200] = {0};
    int path_length = 0;
    struct http_parser_url url_parser;
    http_parser_url_init(&url_parser);

    bool keep_alive = false;
    bool body_ready = false;

    BLASTER_HTTP_REQUEST request = {path, &path_length, &url_parser, &keep_alive, &body_ready};

    http_parser_settings settings;
    http_parser_settings_init(&settings);
    settings.on_url = on_url_ready;
    settings.on_headers_complete = on_headers_ready;
    settings.on_message_complete = on_body_ready;

    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = &request;

    int64_t request_death_ts = now() + 10*1000; // 10 second lifetime
    int64_t last_wakeup = 0;

    while(true) {
        int64_t current_ts = now();
        if (current_ts > request_death_ts) {
            DEBUG_PRINTF("We have used up 10 seconds on this client. Closing...\n");
            break;
        }
        char buf[2048];
        size_t num_bytes_read = tcprecv(client, buf, sizeof(buf), now() + 1);
        if (errno == ECONNRESET) {
            DEBUG_PRINTF("Connection reset, %d requests left\n", requests_left);
            break;
        }
        if(num_bytes_read > 0) {
            last_wakeup = current_ts;
            http_parser_execute(&parser, &settings, buf, num_bytes_read);
        } else if (current_ts - last_wakeup >= 5*1000 && keep_alive) {
            DEBUG_PRINTF("Connection idle for more than 5 seconds. Flushing and closing.\n");
            tcpflush(client, -1);
            break;
        }
        if(body_ready) {
            // if (path_length > 0) {
                // if(keep_alive) {
                //     tcpsend(client, keep_alive_capable, sizeof(keep_alive_capable), -1);
                //     goto reuse;
                // }
                tcpsend(client, no_keep_alive, sizeof(no_keep_alive), -1);
            // }
            yield();
            tcpflush(client, -1);
            break;
        }
    }

    close:
        tcpclose(client);
    return;
    // reuse:
    //     tcpflush(client, -1);
    //     yield();
    //     handle_request(client, requests_left-1);
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
    tcpsock server_socket = tcplisten(address, 300);
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
    printf("Listening on port %d\n", port);

    // Event loop
    // Any time our server_socket is ready, check if the client socket is good
    // the launch a fiber to deal with it.
    while(true) {
        int64_t deadline = now() + 5;
        tcpsock client_tunnel = tcpaccept(server_socket, deadline);
        if (client_tunnel == NULL) {
            continue;
        }
        go(handle_request(client_tunnel, 20));
    }
    return 0;
}

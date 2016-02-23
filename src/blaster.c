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
char keep_alive_capable[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nKeep-Alive: timeout=15, max=200\r\nConnection: keep-alive\r\n\r\nOk\n";

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
coroutine void handle_request(tcpsock client) {
    int64_t connection_last_data_time = now();
    char path[2048] = {0};
    char buf[8192];
    struct http_parser_url url_parser = {};
    int path_length;

    bool keep_alive_request = false;
    bool body_ready = false;

    BLASTER_HTTP_REQUEST request = {path, &path_length, &url_parser, &keep_alive_request, &body_ready};
    int reqs_left = 199;
    size_t num_bytes;
    http_parser_settings settings;
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);

    parser.data = &request;
    settings.on_url = on_url_ready;
    settings.on_headers_complete = on_headers_ready;
    settings.on_message_complete = on_body_ready;

    int64_t request_time_start, current_ts;

    // Launch point for handling another HTTP request.
    begin:
    path_length = -1;
    request_time_start = now();

    // Receive and parse data until we've read the body:
    do {
        current_ts = now();
        // Set a deadline of receiving any data within 5ms.
        num_bytes = tcprecv(client, buf, sizeof(buf), current_ts + 5);
        // Oops, we lost the connection. Clear it out.
        if (errno == ECONNRESET) {
            goto cleanup;
        }

        if (num_bytes > 0) {
            // Parse some data and update the idle time stamp.
            connection_last_data_time = now();
            http_parser_execute(&parser, &settings, buf, num_bytes);
            // signal that other coroutines may execute
            yield();
        } else {
            // We have nothing received yet. Do a sanity check:

            // Have we spent an inordinately long amount of time on this connection (100sec)?
            if (current_ts - request_time_start > 100*1000) {
                DEBUG_PRINTF("Closed by num_bytes == 0 and gave 100 seconds to do something\n");
                goto cleanup;
            }
            // Have we been idle too long?
            if (keep_alive_request && current_ts - connection_last_data_time > 15*1000) {
                DEBUG_PRINTF("Too long idle on a keepalive connection!\n");
                goto cleanup;
            }
        }
    } while(!body_ready);

    // Handle the case where we found a valid HTTP path definition:
    if (path_length > -1) {
        // Special case: Keep alives are ground hogs day -- we have to cycle around and
        // do it all over again:

        if (keep_alive_request) {
            if (now() - connection_last_data_time > 15*1000) {
                DEBUG_PRINTF("Too long idle on a keepalive connection after parsing!\n");
                goto cleanup;
            }
            if (!reqs_left) {
                // DEBUG_PRINTF("No requests left!\n");
                goto cleanup;
            }
            tcpsend(client, keep_alive_capable, sizeof(keep_alive_capable), -1);
            tcpflush(client, -1);
            // The connection is no longer idle:
            connection_last_data_time = now();
            reqs_left--;
            goto begin;
        }

        // Not a keep alive connection, so just send it out and go!
        tcpsend(client, no_keep_alive, sizeof(no_keep_alive), -1);
        tcpflush(client, -1);
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
        tcpsock client_tunnel = tcpaccept(server_socket, -1);
        if (client_tunnel == NULL)
            continue;
        go(handle_request(client_tunnel));
    }
    return 0;
}

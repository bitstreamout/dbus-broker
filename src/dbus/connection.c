/*
 * Connection
 */

#include <c-macro.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include "dbus/connection.h"
#include "dbus/message.h"
#include "dbus/socket.h"
#include "user.h"
#include "util/dispatch.h"
#include "util/error.h"

static int connection_init(Connection *connection,
                           DispatchContext *dispatch_ctx,
                           DispatchFn dispatch_fn,
                           User *user,
                           int fd) {
        int r;

        *connection = (Connection)CONNECTION_NULL(*connection);
        connection->user = user_ref(user);

        r = socket_init(&connection->socket, fd);
        if (r)
                return error_fold(r);

        r = dispatch_file_init(&connection->socket_file,
                               dispatch_ctx,
                               dispatch_fn,
                               fd,
                               EPOLLHUP | EPOLLIN | EPOLLOUT);
        if (r)
                return error_fold(r);

        return 0;
}

/**
 * connection_init_server() - XXX
 */
int connection_init_server(Connection *connection,
                           DispatchContext *dispatch_ctx,
                           DispatchFn dispatch_fn,
                           User *user,
                           const char *guid,
                           int fd) {
        _c_cleanup_(connection_deinitp) Connection *c = connection;
        int r;

        r = connection_init(c,
                            dispatch_ctx,
                            dispatch_fn,
                            user,
                            fd);
        if (r)
                return error_trace(r);

        c->server = true;
        sasl_server_init(&c->sasl.server, user->uid, guid);
        c = NULL;
        return 0;
}

/**
 * connection_init_client() - XXX
 */
int connection_init_client(Connection *connection,
                           DispatchContext *dispatch_ctx,
                           DispatchFn dispatch_fn,
                           User *user,
                           int fd) {
        _c_cleanup_(connection_deinitp) Connection *c = connection;
        int r;

        r = connection_init(c,
                            dispatch_ctx,
                            dispatch_fn,
                            user,
                            fd);
        if (r)
                return error_trace(r);

        c->server = false;
        sasl_client_init(&c->sasl.client);
        c = NULL;
        return 0;
}

/**
 * connection_deinit() - XXX
 */
void connection_deinit(Connection *connection) {
        if (connection->server)
                sasl_server_deinit(&connection->sasl.server);
        else
                sasl_client_deinit(&connection->sasl.client);
        dispatch_file_deinit(&connection->socket_file);
        socket_deinit(&connection->socket);
        connection->user = user_unref(connection->user);
}

/**
 * connection_open() - XXX
 */
int connection_open(Connection *connection) {
        uint32_t events = EPOLLHUP | EPOLLIN;
        const char *request;
        size_t n_request;
        int r;

        assert(socket_is_running(&connection->socket));

        if (!connection->server) {
                r = sasl_client_dispatch(&connection->sasl.client, NULL, 0, &request, &n_request);
                if (r)
                        return error_fold(r);

                if (request) {
                        r = socket_queue_line(&connection->socket, request, n_request);
                        if (r)
                                return error_fold(r);

                        events |= EPOLLOUT;
                }
        }

        dispatch_file_select(&connection->socket_file, events);
        return 0;
}

/**
 * connection_shutdown() - XXX
 */
void connection_shutdown(Connection *connection) {
        socket_shutdown(&connection->socket);
}

/**
 * connection_close() - XXX
 */
void connection_close(Connection *connection) {
        dispatch_file_deselect(&connection->socket_file, EPOLLIN);
        socket_close(&connection->socket);
}

/**
 * connection_dispatch() - XXX
 */
int connection_dispatch(Connection *connection, uint32_t event) {
        int r;

        r = socket_dispatch(&connection->socket, event);
        if (!r)
                dispatch_file_clear(&connection->socket_file, event);
        else if (r == SOCKET_E_LOST_INTEREST)
                dispatch_file_deselect(&connection->socket_file, event);
        else if (r != SOCKET_E_PREEMPTED)
                return error_fold(r);

        return 0;
}

static int connection_feed_sasl(Connection *connection, const char *input, size_t n_input) {
        const char *output;
        size_t n_output;
        int r;

        assert(!connection->authenticated);

        if (connection->server) {
                r = sasl_server_dispatch(&connection->sasl.server, input, n_input, &output, &n_output);
                if (r > 0)
                        return CONNECTION_E_RESET;
                else if (r < 0)
                        return error_fold(r);

                connection->authenticated = sasl_server_is_done(&connection->sasl.server);
        } else {
                r = sasl_client_dispatch(&connection->sasl.client, input, n_input, &output, &n_output);
                if (r > 0)
                        return CONNECTION_E_RESET;
                else if (r < 0)
                        return error_fold(r);

                connection->authenticated = sasl_client_is_done(&connection->sasl.client);
        }

        if (output) {
                r = socket_queue_line(&connection->socket, output, n_output);
                if (r)
                        return error_fold(r);

                dispatch_file_select(&connection->socket_file, EPOLLOUT);
        }

        return 0;
}

/**
 * connection_dequeue() - XXX
 */
int connection_dequeue(Connection *connection, Message **messagep) {
        const char *input;
        size_t n_input;
        int r;

        if (_c_unlikely_(!connection->authenticated)) {
                do {
                        r = socket_dequeue_line(&connection->socket, &input, &n_input);
                        if (r == SOCKET_E_RESET)
                                return CONNECTION_E_RESET;
                        else if (r == SOCKET_E_EOF)
                                return CONNECTION_E_EOF;
                        else if (r)
                                return error_fold(r);
                        else if (!input) {
                                *messagep = NULL;
                                return 0;
                        }

                        r = connection_feed_sasl(connection, input, n_input);
                        if (r)
                                return error_trace(r);
                } while (!connection->authenticated);
        }

        r = socket_dequeue(&connection->socket, messagep);
        if (r == SOCKET_E_RESET)
                return CONNECTION_E_RESET;
        else if (r == SOCKET_E_EOF)
                return CONNECTION_E_EOF;
        else
                return error_fold(r);
}

/**
 * connection_queue() - XXX
 */
int connection_queue(Connection *connection, uint64_t transaction_id, Message *message) {
        SocketBuffer *skb;
        int r;

        if (transaction_id) {
                if (transaction_id == connection->transaction_id) {
                        /* this connection already received this message */
                        return 0;
                } else {
                        assert(connection->transaction_id < transaction_id);
                        connection->transaction_id = transaction_id;
                }
        }

        r = socket_buffer_new_message(&skb, message);
        if (r)
                return error_fold(r);

        socket_queue(&connection->socket, skb);
        if (socket_has_output(&connection->socket))
                dispatch_file_select(&connection->socket_file, EPOLLOUT);

        return 0;
}

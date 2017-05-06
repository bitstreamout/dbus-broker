/*
 * Broker Controller
 */

#include <c-dvar.h>
#include <c-dvar-type.h>
#include <c-macro.h>
#include <c-string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "broker/controller.h"
#include "bus.h"
#include "dbus/connection.h"
#include "dbus/message.h"
#include "dbus/protocol.h"
#include "dbus/socket.h"
#include "dbus/unique-name.h"
#include "util/error.h"
#include "util/fdlist.h"

typedef struct DispatchContext DispatchContext;
typedef struct ControllerMethod ControllerMethod;
typedef int (*ControllerMethodFn) (Bus *bus, DispatchContext *dispatcher, CDVar *var_in, FDList *fds_in, CDVar *var_out);

struct ControllerMethod {
        const char *name;
        ControllerMethodFn fn;
        const CDVarType *in;
        const CDVarType *out;
};

/*
 * This macro defines a c-dvar type for DBus Messages. It evaluates to:
 *
 *         ((yyyyuua(yv))X)
 *
 * ..where 'X' is provided via @_body. That is, it evaluates to the combination
 * of DBus Header and DBus Body for a given body-type.
 */
#define CONTROLLER_T_MESSAGE(_body) \
        C_DVAR_T_TUPLE2(                                \
                C_DVAR_T_TUPLE7(                        \
                        C_DVAR_T_y,                     \
                        C_DVAR_T_y,                     \
                        C_DVAR_T_y,                     \
                        C_DVAR_T_y,                     \
                        C_DVAR_T_u,                     \
                        C_DVAR_T_u,                     \
                        C_DVAR_T_ARRAY(                 \
                                C_DVAR_T_TUPLE2(        \
                                        C_DVAR_T_y,     \
                                        C_DVAR_T_v      \
                                )                       \
                        )                               \
                ),                                      \
                _body                                   \
        )

static const CDVarType controller_type_in_h[] = {
        C_DVAR_T_INIT(
                C_DVAR_T_TUPLE1(
                        C_DVAR_T_h
                )
        )
};
static const CDVarType controller_type_out_unit[] = {
        C_DVAR_T_INIT(
                CONTROLLER_T_MESSAGE(
                        C_DVAR_T_TUPLE0
                )
        )
};

static void controller_dvar_write_signature_out(CDVar *var, const CDVarType *type) {
        char signature[C_DVAR_TYPE_LENGTH_MAX + 1];

        assert(type->length < sizeof(signature) + strlen("((yyyyuua(yv))())"));
        assert(type[0].element == '(');
        assert(type[1].element == '(');
        assert(type[2].element == 'y');
        assert(type[3].element == 'y');
        assert(type[4].element == 'y');
        assert(type[5].element == 'y');
        assert(type[6].element == 'u');
        assert(type[7].element == 'u');
        assert(type[8].element == 'a');
        assert(type[9].element == '(');
        assert(type[10].element == 'y');
        assert(type[11].element == 'v');
        assert(type[12].element == ')');
        assert(type[13].element == ')');
        assert(type[14].element == '(');
        assert(type[type->length - 2].element == ')');
        assert(type[type->length - 1].element == ')');

        for (unsigned int i = strlen("((yyyyuua(yv))("), j = 0; i < type->length - strlen("))"); i++, j++)
                signature[j] = type[i].element;

        signature[type->length - strlen("((yyyyuua(yv))())")] = '\0';

        c_dvar_write(var, "g", signature);
}

static int controller_dvar_verify_signature_in(const CDVarType *type, const char *signature) {
        if (type->length != strlen(signature) + 2)
                return CONTROLLER_E_UNEXPECTED_SIGNATURE;

        assert(type[0].element == '(');
        assert(type[type->length - 1].element == ')');

        for (unsigned int i = 1; i + 1 < type->length; i++)
                if (signature[i - 1] != type[i].element)
                        return CONTROLLER_E_UNEXPECTED_SIGNATURE;

        return 0;
}

static void controller_write_reply_header(CDVar *var, uint32_t serial, const CDVarType *type) {
        c_dvar_write(var, "(yyyyuu[(y<u>)(y<",
                     c_dvar_is_big_endian(var) ? 'B' : 'l', DBUS_MESSAGE_TYPE_METHOD_RETURN, DBUS_HEADER_FLAG_NO_REPLY_EXPECTED, 1, 0, (uint32_t)-1,
                     DBUS_MESSAGE_FIELD_REPLY_SERIAL, c_dvar_type_u, serial,
                     DBUS_MESSAGE_FIELD_SIGNATURE, c_dvar_type_g);
        controller_dvar_write_signature_out(var, type);
        c_dvar_write(var, ">)])");
}

/*
static void controller_write_signal_header(CDVar *var, const char *member, const char *signature) {
        c_dvar_write(var, "(yyyyuu[(y<o>)(y<s>)(y<s>)(y<g>)])",
                     c_dvar_is_big_endian(var) ? 'B' : 'l', DBUS_MESSAGE_TYPE_SIGNAL, DBUS_HEADER_FLAG_NO_REPLY_EXPECTED, 1, 0, (uint32_t)-1,
                     DBUS_MESSAGE_FIELD_PATH, c_dvar_type_o, "/org/bus1/DBus/Controller",
                     DBUS_MESSAGE_FIELD_INTERFACE, c_dvar_type_s, "org.bus1.Controller",
                     DBUS_MESSAGE_FIELD_MEMBER, c_dvar_type_s, member,
                     DBUS_MESSAGE_FIELD_SIGNATURE, c_dvar_type_g, signature);
}
*/

static int controller_send_error(Connection *connection, uint32_t serial, const char *error) {
        static const CDVarType type[] = {
                C_DVAR_T_INIT(
                        CONTROLLER_T_MESSAGE(
                                C_DVAR_T_TUPLE0
                        )
                )
        };
        _c_cleanup_(c_dvar_deinitp) CDVar var = C_DVAR_INIT;
        _c_cleanup_(message_unrefp) Message *message = NULL;
        void *data;
        size_t n_data;
        int r;

        c_dvar_begin_write(&var, type, 1);
        c_dvar_write(&var, "((yyyyuu[(y<u>)(y<s>)])())",
                     c_dvar_is_big_endian(&var) ? 'B' : 'l', DBUS_MESSAGE_TYPE_ERROR, DBUS_HEADER_FLAG_NO_REPLY_EXPECTED, 1, 0, (uint32_t)-1,
                     DBUS_MESSAGE_FIELD_REPLY_SERIAL, c_dvar_type_u, serial,
                     DBUS_MESSAGE_FIELD_ERROR_NAME, c_dvar_type_s, error);

        r = c_dvar_end_write(&var, &data, &n_data);
        if (r)
                return error_origin(r);

        r = message_new_outgoing(&message, data, n_data);
        if (r)
                return error_fold(r);

        r = connection_queue_message(connection, message);
        if (r)
                return error_fold(r);

        return 0;
}

static int controller_end_read(CDVar *var) {
        int r;

        r = c_dvar_end_read(var);
        switch (r) {
        case C_DVAR_E_CORRUPT_DATA:
        case C_DVAR_E_OUT_OF_BOUNDS:
        case C_DVAR_E_TYPE_MISMATCH:
                return CONTROLLER_E_INVALID_MESSAGE;
        default:
                return error_origin(r);
        }
}

static int controller_method_add_listener(Bus *bus, DispatchContext *dispatcher, CDVar *in_v, FDList *fds, CDVar *out_v) {
        Listener *listener;
        uint32_t fd_index;
        int r, fd;

        c_dvar_read(in_v, "(h)", &fd_index);

        r = controller_end_read(in_v);
        if (r)
                return error_trace(r);

        /* XXX: error handling */
        fd = fdlist_get(fds, fd_index);

        r = listener_new_with_fd(&listener, bus, dispatcher, fd);
        if (r)
                return error_fold(r);

        fdlist_steal(fds, fd_index);

        c_dvar_write(out_v, "()");

        return 0;
}

static int controller_handle_method(const ControllerMethod *method, Bus *bus, Connection *connection, const char *path, uint32_t serial, const char *signature_in, Message *message_in) {
        _c_cleanup_(c_dvar_deinitp) CDVar var_in = C_DVAR_INIT, var_out = C_DVAR_INIT;
        _c_cleanup_(message_unrefp) Message *message_out = NULL;
        void *data;
        size_t n_data;
        int r;

        /*
         * Verify the input signature and prepare the input & output variants
         * for input parsing and output marshaling.
         */

        r = controller_dvar_verify_signature_in(method->in, signature_in);
        if (r)
                return error_trace(r);

        c_dvar_begin_read(&var_in, message_in->big_endian, method->in, 1, message_in->body, message_in->n_body);
        c_dvar_begin_write(&var_out, method->out, 1);

        /*
         * Write the generic reply-header and then call into the method-handler
         * of the specific controller method. Note that the controller-methods are
         * responsible to call controller_end_read(var_in), to verify all read data
         * was correct.
         */

        c_dvar_write(&var_out, "(");
        controller_write_reply_header(&var_out, serial, method->out);

        r = method->fn(bus, connection->socket_file.context, &var_in, message_in->fds, &var_out);
        if (r)
                return error_trace(r);

        c_dvar_write(&var_out, ")");

        /*
         * The message was correctly handled and the reply is serialized in
         * @var_out. Lets finish it up and queue the reply on the destination.
         * Note that any failure in doing so must be a fatal error, so there is
         * no point in reverting the operation on failure.
         */

        r = c_dvar_end_write(&var_out, &data, &n_data);
        if (r)
                return error_origin(r);

        r = message_new_outgoing(&message_out, data, n_data);
        if (r)
                return error_fold(r);

        r = connection_queue_message(connection, message_out);
        if (r)
                return error_fold(r);

        return 0;
}

static int controller_dispatch_method(Bus *bus, Connection *connection, uint32_t serial, const char *method, const char *path, const char *signature, Message *message) {
        static const ControllerMethod methods[] = {
                { "AddListener",        controller_method_add_listener,         controller_type_in_h,   controller_type_out_unit },
        };

        for (size_t i = 0; i < C_ARRAY_SIZE(methods); i++) {
                if (strcmp(methods[i].name, method) != 0)
                        continue;

                return controller_handle_method(&methods[i], bus, connection, path, serial, signature, message);
        }

        return CONTROLLER_E_UNEXPECTED_METHOD;
}

static int controller_dispatch_interface(Bus *bus, Connection *connection, uint32_t serial, const char *interface, const char *member, const char *path, const char *signature, Message *message) {
        if (message->header->type != DBUS_MESSAGE_TYPE_METHOD_CALL)
                /* XXX: ignore, like in the driver? */
                return 0;

        if (interface && _c_unlikely_(strcmp(interface, "org.bus1.Controller") != 0))
                return CONTROLLER_E_UNEXPECTED_INTERFACE;
        if (_c_unlikely_(strcmp(path, "/org/bus1/Controller") != 0))
                return CONTROLLER_E_UNEXPECTED_PATH;

        return controller_dispatch_method(bus, connection, serial, member, path, signature, message);
}

int controller_dispatch(Bus *bus, Connection *connection, Message *message) {
        MessageMetadata metadata;
        const char *signature;
        int r;

        r = message_parse_metadata(message, &metadata);
        if (r > 0)
                return CONTROLLER_E_DISCONNECT;
        else if (r < 0)
                return error_fold(r);

        /* no signature implies empty signature */
        signature = metadata.fields.signature ?: "";

        r = controller_dispatch_interface(bus,
                                          connection,
                                          metadata.header.serial,
                                          metadata.fields.interface,
                                          metadata.fields.member,
                                          metadata.fields.path,
                                          signature,
                                          message);
        switch (r) {
        case CONTROLLER_E_INVALID_MESSAGE:
                return CONTROLLER_E_DISCONNECT;
        case CONTROLLER_E_UNEXPECTED_PATH:
        case CONTROLLER_E_UNEXPECTED_MESSAGE_TYPE:
                r = controller_send_error(connection, metadata.header.serial, "org.freedesktop.DBus.Error.AccessDenied");
                break;
        case CONTROLLER_E_UNEXPECTED_INTERFACE:
                r = controller_send_error(connection, metadata.header.serial, "org.freedesktop.DBus.Error.UnknownInterface");
                break;
        case CONTROLLER_E_UNEXPECTED_METHOD:
                r = controller_send_error(connection, metadata.header.serial, "org.freedesktop.DBus.Error.UnknownMethod");
                break;
        case CONTROLLER_E_UNEXPECTED_SIGNATURE:
                r = controller_send_error(connection, metadata.header.serial, "org.freedesktop.DBus.Error.InvalidArgs");
                break;
        default:
                break;
        }

        return error_trace(r);
}

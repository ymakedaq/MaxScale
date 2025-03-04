/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#define MXS_MODULE_NAME "MySQLBackend"

#include <maxscale/protocol/mysql.h>
#include <maxscale/limits.h>
#include <maxscale/log_manager.h>
#include <maxscale/modutil.h>
#include <maxscale/utils.h>
#include <mysqld_error.h>
#include <maxscale/alloc.h>
#include <maxscale/modinfo.h>
#include <maxscale/protocol.h>

/*
 * MySQL Protocol module for handling the protocol between the gateway
 * and the backend MySQL database.
 *
 * Revision History
 * Date         Who                     Description
 * 14/06/2013   Mark Riddoch            Initial version
 * 17/06/2013   Massimiliano Pinto      Added MaxScale To Backends routines
 * 01/07/2013   Massimiliano Pinto      Put Log Manager example code behind SS_DEBUG macros.
 * 03/07/2013   Massimiliano Pinto      Added delayq for incoming data before mysql connection
 * 04/07/2013   Massimiliano Pinto      Added asynchronous MySQL protocol connection to backend
 * 05/07/2013   Massimiliano Pinto      Added closeSession if backend auth fails
 * 12/07/2013   Massimiliano Pinto      Added Mysql Change User via dcb->func.auth()
 * 15/07/2013   Massimiliano Pinto      Added Mysql session change via dcb->func.session()
 * 17/07/2013   Massimiliano Pinto      Added dcb->command update from gwbuf->command for proper routing
 *                                      server replies to client via router->clientReply
 * 04/09/2013   Massimiliano Pinto      Added dcb->session and dcb->session->client checks for NULL
 * 12/09/2013   Massimiliano Pinto      Added checks in gw_read_backend_event() for gw_read_backend_handshake
 * 27/09/2013   Massimiliano Pinto      Changed in gw_read_backend_event the check for dcb_read(),
 *                                      now is if rc less than 0
 * 24/10/2014   Massimiliano Pinto      Added Mysql user@host @db authentication support
 * 10/11/2014   Massimiliano Pinto      Client charset is passed to backend
 * 19/06/2015   Martin Brampton         Persistent connection handling
 * 07/10/2015   Martin Brampton         Remove calls to dcb_close - should be done by routers
 * 27/10/2015   Martin Brampton         Test for RCAP_TYPE_NO_RSESSION before calling clientReply
 * 23/05/2016   Martin Brampton         Provide for backend SSL
 *
 */

static int gw_create_backend_connection(DCB *backend, SERVER *server, MXS_SESSION *in_session);
static int gw_read_backend_event(DCB* dcb);
static int gw_write_backend_event(DCB *dcb);
static int gw_MySQLWrite_backend(DCB *dcb, GWBUF *queue);
static int gw_error_backend_event(DCB *dcb);
static int gw_backend_close(DCB *dcb);
static int gw_backend_hangup(DCB *dcb);
static int backend_write_delayqueue(DCB *dcb, GWBUF *buffer);
static void backend_set_delayqueue(DCB *dcb, GWBUF *queue);
static int gw_change_user(DCB *backend_dcb, SERVER *server, MXS_SESSION *in_session, GWBUF *queue);
static char *gw_backend_default_auth();
static GWBUF* process_response_data(DCB* dcb, GWBUF** readbuf, int nbytes_to_process);
extern char* create_auth_failed_msg(GWBUF* readbuf, char* hostaddr, uint8_t* sha1);
static bool sescmd_response_complete(DCB* dcb);
static void gw_reply_on_error(DCB *dcb, mxs_auth_state_t state);
static int gw_read_and_write(DCB *dcb);
static int gw_decode_mysql_server_handshake(MySQLProtocol *conn, uint8_t *payload);
static int gw_do_connect_to_backend(char *host, int port, int *fd);
static void inline close_socket(int socket);
static GWBUF *gw_create_change_user_packet(MYSQL_session*  mses,
                                           MySQLProtocol*  protocol);
static int gw_send_change_user_to_backend(char          *dbname,
                                          char          *user,
                                          uint8_t       *passwd,
                                          MySQLProtocol *conn);
static bool gw_connection_established(DCB* dcb);

/*
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_PROTOCOL MyObject =
    {
        gw_read_backend_event,      /* Read - EPOLLIN handler        */
        gw_MySQLWrite_backend,      /* Write - data from gateway     */
        gw_write_backend_event,     /* WriteReady - EPOLLOUT handler */
        gw_error_backend_event,     /* Error - EPOLLERR handler      */
        gw_backend_hangup,          /* HangUp - EPOLLHUP handler     */
        NULL,                       /* Accept                        */
        gw_create_backend_connection, /* Connect                     */
        gw_backend_close,           /* Close                         */
        NULL,                       /* Listen                        */
        gw_change_user,             /* Authentication                */
        NULL,                       /* Session                       */
        gw_backend_default_auth,    /* Default authenticator         */
        NULL,                       /* Connection limit reached      */
        gw_connection_established
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_PROTOCOL,
        MXS_MODULE_GA,
        MXS_PROTOCOL_VERSION,
        "The MySQL to backend server protocol",
        "V2.0.0",
        &MyObject,
        NULL, /* Process init. */
        NULL, /* Process finish. */
        NULL, /* Thread init. */
        NULL, /* Thread finish. */
        {
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}

/**
 * The default authenticator name for this protocol
 *
 * This is not used for a backend protocol, it is for client authentication.
 *
 * @return name of authenticator
 */
static char *gw_backend_default_auth()
{
    return "MySQLBackendAuth";
}
/*lint +e14 */

/*******************************************************************************
 *******************************************************************************
 *
 * API Entry Point - Connect
 *
 * This is the first entry point that will be called in the life of a backend
 * (database) connection. It creates a protocol data structure and attempts
 * to open a non-blocking socket to the database. If it succeeds, the
 * protocol_auth_state will become MYSQL_CONNECTED.
 *
 *******************************************************************************
 ******************************************************************************/

/*
 * Create a new backend connection.
 *
 * This routine will connect to a backend server and it is called by dbc_connect
 * in router->newSession
 *
 * @param backend_dcb, in, out, use - backend DCB allocated from dcb_connect
 * @param server, in, use - server to connect to
 * @param session, in use - current session from client DCB
 * @return 0/1 on Success and -1 on Failure.
 * If succesful, returns positive fd to socket which is connected to
 *  backend server. Positive fd is copied to protocol and to dcb.
 * If fails, fd == -1 and socket is closed.
 */
static int gw_create_backend_connection(DCB *backend_dcb,
                                        SERVER *server,
                                        MXS_SESSION *session)
{
    MySQLProtocol *protocol = NULL;
    int rv = -1;
    int fd = -1;

    protocol = mysql_protocol_init(backend_dcb, -1);
    ss_dassert(protocol != NULL);

    if (protocol == NULL)
    {
        MXS_DEBUG("%lu [gw_create_backend_connection] Failed to create "
                  "protocol object for backend connection.",
                  pthread_self());
        MXS_ERROR("Failed to create protocol object for backend connection.");
        goto return_fd;
    }

    /** Copy client flags to backend protocol */
    if (backend_dcb->session->client_dcb->protocol)
    {
        MySQLProtocol *client = (MySQLProtocol*)backend_dcb->session->client_dcb->protocol;
        protocol->client_capabilities = client->client_capabilities;
        protocol->charset = client->charset;
        protocol->extra_capabilities = client->extra_capabilities;
    }
    else
    {
        protocol->client_capabilities = (int)GW_MYSQL_CAPABILITIES_CLIENT;
        protocol->charset = 0x08;
    }

    /*< if succeed, fd > 0, -1 otherwise */
    /* TODO: Better if function returned a protocol auth state */
    rv = gw_do_connect_to_backend(server->name, server->port, &fd);
    /*< Assign protocol with backend_dcb */
    backend_dcb->protocol = protocol;

    /*< Set protocol state */
    switch (rv)
    {
    case 0:
        ss_dassert(fd > 0);
        protocol->fd = fd;
        protocol->protocol_auth_state = MXS_AUTH_STATE_CONNECTED;
        MXS_DEBUG("%lu [gw_create_backend_connection] Established "
                  "connection to %s:%i, protocol fd %d client "
                  "fd %d.",
                  pthread_self(),
                  server->name,
                  server->port,
                  protocol->fd,
                  session->client_dcb->fd);
        break;

    case 1:
        /* The state MYSQL_PENDING_CONNECT is likely to be transitory,    */
        /* as it means the calls have been successful but the connection  */
        /* has not yet completed and the calls are non-blocking.          */
        ss_dassert(fd > 0);
        protocol->protocol_auth_state = MXS_AUTH_STATE_PENDING_CONNECT;
        protocol->fd = fd;
        MXS_DEBUG("%lu [gw_create_backend_connection] Connection "
                  "pending to %s:%i, protocol fd %d client fd %d.",
                  pthread_self(),
                  server->name,
                  server->port,
                  protocol->fd,
                  session->client_dcb->fd);
        break;

    default:
        /* Failure - the state reverts to its initial value */
        ss_dassert(fd == -1);
        ss_dassert(protocol->protocol_auth_state == MXS_AUTH_STATE_INIT);
        MXS_DEBUG("%lu [gw_create_backend_connection] Connection "
                  "failed to %s:%i, protocol fd %d client fd %d.",
                  pthread_self(),
                  server->name,
                  server->port,
                  protocol->fd,
                  session->client_dcb->fd);
        break;
    } /*< switch */

return_fd:
    return fd;
}

/**
 * gw_do_connect_to_backend
 *
 * This routine creates socket and connects to a backend server.
 * Connect it non-blocking operation. If connect fails, socket is closed.
 *
 * @param host The host to connect to
 * @param port The host TCP/IP port
 * @param *fd where connected fd is copied
 * @return 0/1 on success and -1 on failure
 * If successful, fd has file descriptor to socket which is connected to
 * backend server. In failure, fd == -1 and socket is closed.
 *
 */
static int gw_do_connect_to_backend(char *host, int port, int *fd)
{
    struct sockaddr_storage serv_addr = {};
    int rv = -1;

    /* prepare for connect */
    int so = open_network_socket(MXS_SOCKET_NETWORK, &serv_addr, host, port);

    if (so == -1)
    {
        MXS_ERROR("Establishing connection to backend server [%s]:%d failed.", host, port);
        return rv;
    }

    rv = connect(so, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    if (rv != 0)
    {
        if (errno == EINPROGRESS)
        {
            rv = 1;
        }
        else
        {
            MXS_ERROR("Failed to connect backend server [%s]:%d due to: %d, %s.",
                      host, port, errno, mxs_strerror(errno));
            close(so);
            return rv;
        }
    }

    *fd = so;
    MXS_DEBUG("%lu [gw_do_connect_to_backend] Connected to backend server "
              "[%s]:%d, fd %d.", pthread_self(), host, port, so);

    return rv;

}

/**
 * @brief Check if the response contain an error
 *
 * @param buffer Buffer with a complete response
 * @return True if the reponse contains an MySQL error packet
 */
bool is_error_response(GWBUF *buffer)
{
    uint8_t cmd;
    return gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd) && cmd == MYSQL_REPLY_ERR;
}

/**
 * @brief Log handshake failure
 *
 * @param dcb Backend DCB where authentication failed
 * @param buffer Buffer containing the response from the backend
 */
static void handle_error_response(DCB *dcb, GWBUF *buffer)
{
    uint8_t *data = (uint8_t*)GWBUF_DATA(buffer);
    size_t len = MYSQL_GET_PAYLOAD_LEN(data);
    uint16_t errcode = MYSQL_GET_ERRCODE(data);
    char bufstr[len];
    memcpy(bufstr, data + 7, len - 3);
    bufstr[len - 3] = '\0';

    MXS_ERROR("Invalid authentication message from backend '%s'. Error code: %d, "
              "Msg : %s", dcb->server->unique_name, errcode, bufstr);

    /** If the error is ER_HOST_IS_BLOCKED put the server into maintenace mode.
     * This will prevent repeated authentication failures. */
    if (errcode == ER_HOST_IS_BLOCKED)
    {
        MXS_ERROR("Server %s has been put into maintenance mode due "
                  "to the server blocking connections from MaxScale. "
                  "Run 'mysqladmin -h %s -P %d flush-hosts' on this "
                  "server before taking this server out of maintenance "
                  "mode.", dcb->server->unique_name,
                  dcb->server->name, dcb->server->port);

        server_set_status(dcb->server, SERVER_MAINT);
    }
    else if (errcode == ER_ACCESS_DENIED_ERROR ||
             errcode == ER_DBACCESS_DENIED_ERROR ||
             errcode == ER_ACCESS_DENIED_NO_PASSWORD_ERROR)
    {
        if (dcb->session->state != SESSION_STATE_DUMMY)
        {
            // Authentication failed, reload users
            service_refresh_users(dcb->service);
        }
    }
}

/**
 * @brief Handle the server's response packet
 *
 * This function reads the server's response packet and does the final step of
 * the authentication.
 *
 * @param dcb Backend DCB
 * @param buffer Buffer containing the server's complete handshake
 * @return MXS_AUTH_STATE_HANDSHAKE_FAILED on failure.
 */
mxs_auth_state_t handle_server_response(DCB *dcb, GWBUF *buffer)
{
    MySQLProtocol *proto = (MySQLProtocol*)dcb->protocol;
    mxs_auth_state_t rval = proto->protocol_auth_state == MXS_AUTH_STATE_CONNECTED ?
                            MXS_AUTH_STATE_HANDSHAKE_FAILED : MXS_AUTH_STATE_FAILED;

    int rc = dcb->authfunc.extract(dcb, buffer);

    if (rc == MXS_AUTH_SUCCEEDED || rc == MXS_AUTH_INCOMPLETE)
    {
        switch (dcb->authfunc.authenticate(dcb))
        {
        case MXS_AUTH_INCOMPLETE:
        case MXS_AUTH_SSL_INCOMPLETE:
            rval = MXS_AUTH_STATE_RESPONSE_SENT;
            break;

        case MXS_AUTH_SUCCEEDED:
            rval = MXS_AUTH_STATE_COMPLETE;

        default:
            break;
        }
    }

    return rval;
}

/*******************************************************************************
 *******************************************************************************
 *
 * API Entry Point - Read
 *
 * When the polling mechanism finds that new incoming data is available for
 * a backend connection, it will call this entry point, passing the relevant
 * DCB.
 *
 * The first time through, it is expected that protocol_auth_state will be
 * MYSQL_CONNECTED and an attempt will be made to send authentication data
 * to the backend server. The state may progress to MYSQL_AUTH_REC although
 * for an SSL connection this will not happen straight away, and the state
 * will remain MYSQL_CONNECTED.
 *
 * When the connection is fully established, it is expected that the state
 * will be MYSQL_IDLE and the information read from the backend will be
 * transferred to the client (front end).
 *
 *******************************************************************************
 ******************************************************************************/

/**
 * Backend Read Event for EPOLLIN on the MySQL backend protocol module
 * @param dcb   The backend Descriptor Control Block
 * @return 1 on operation, 0 for no action
 */
static int
gw_read_backend_event(DCB *dcb)
{
    CHK_DCB(dcb);
    if (dcb->persistentstart)
    {
        /** If a DCB gets a read event when it's in the persistent pool, it is
         * treated as if it were an error. */
        dcb->dcb_errhandle_called = true;
        return 0;
    }

    if (dcb->dcb_is_zombie || dcb->session == NULL ||
        dcb->session->state == SESSION_STATE_DUMMY)
    {
        return 0;
    }

    CHK_SESSION(dcb->session);

    MySQLProtocol *proto = (MySQLProtocol *)dcb->protocol;
    CHK_PROTOCOL(proto);

    MXS_DEBUG("%lu [gw_read_backend_event] Read dcb %p fd %d protocol state %d, %s.",
              pthread_self(), dcb, dcb->fd, proto->protocol_auth_state,
              STRPROTOCOLSTATE(proto->protocol_auth_state));

    int rc = 0;
    if (proto->protocol_auth_state == MXS_AUTH_STATE_COMPLETE)
    {
        rc = gw_read_and_write(dcb);
    }
    else
    {
        GWBUF *readbuf = NULL;

        if (!read_complete_packet(dcb, &readbuf))
        {
            proto->protocol_auth_state = MXS_AUTH_STATE_FAILED;
            gw_reply_on_error(dcb, proto->protocol_auth_state);
        }
        else if (readbuf)
        {
            /** We have a complete response from the server */
            /** TODO: add support for non-contiguous responses */
            readbuf = gwbuf_make_contiguous(readbuf);
            MXS_ABORT_IF_NULL(readbuf);

            if (is_error_response(readbuf))
            {
                /** The server responded with an error */
                proto->protocol_auth_state = MXS_AUTH_STATE_FAILED;
                handle_error_response(dcb, readbuf);
            }

            if (proto->protocol_auth_state == MXS_AUTH_STATE_CONNECTED)
            {
                mxs_auth_state_t state = MXS_AUTH_STATE_FAILED;

                /** Read the server handshake and send the standard response */
                if (gw_read_backend_handshake(dcb, readbuf))
                {
                    state = gw_send_backend_auth(dcb);
                }

                proto->protocol_auth_state = state;
            }
            else if (proto->protocol_auth_state == MXS_AUTH_STATE_RESPONSE_SENT)
            {
                /** Read the message from the server. This will be the first
                 * packet that can contain authenticator specific data from the
                 * backend server. For 'mysql_native_password' it'll be an OK
                 * packet */
                proto->protocol_auth_state = handle_server_response(dcb, readbuf);
            }

            if (proto->protocol_auth_state == MXS_AUTH_STATE_COMPLETE)
            {
                /** Authentication completed successfully */
                GWBUF *localq = dcb->delayq;
                dcb->delayq = NULL;

                if (localq)
                {
                    /** Send the queued commands to the backend */
                    rc = backend_write_delayqueue(dcb, localq);
                }
            }
            else if (proto->protocol_auth_state == MXS_AUTH_STATE_FAILED ||
                     proto->protocol_auth_state == MXS_AUTH_STATE_HANDSHAKE_FAILED)
            {
                /** Authentication failed */
                gw_reply_on_error(dcb, proto->protocol_auth_state);
            }

            gwbuf_free(readbuf);
        }
        else if (proto->protocol_auth_state == MXS_AUTH_STATE_CONNECTED &&
                 dcb->ssl_state == SSL_ESTABLISHED)
        {
            proto->protocol_auth_state = gw_send_backend_auth(dcb);
        }
    }

    return rc;
}

/**
 * @brief Authentication of backend - read the reply, or handle an error
 *
 * @param dcb               Descriptor control block for backend server
 * @param local_session     The current MySQL session data structure
 * @return
 */
static void
gw_reply_on_error(DCB *dcb, mxs_auth_state_t state)
{
    MXS_SESSION *session = dcb->session;
    CHK_SESSION(session);

    GWBUF* errbuf = mysql_create_custom_error(1, 0, "Authentication with backend "
                                              "failed. Session will be closed.");

    if (session->router_session)
    {
        bool succp = false;

        session->service->router->handleError(session->service->router_instance,
                                              session->router_session,
                                              errbuf, dcb, ERRACT_REPLY_CLIENT, &succp);

        session->state = SESSION_STATE_STOPPING;
        ss_dassert(dcb->dcb_errhandle_called);
    }
    else
    {
        /** A NULL router_session is valid if a router declares the
         * RCAP_TYPE_NO_RSESSION capability flag */
        dcb->dcb_errhandle_called = true;
    }

    gwbuf_free(errbuf);
}

/**
 * @brief Check if a reply can be routed to the client
 *
 * @param Backend DCB
 * @return True if session is ready for reply routing
 */
static inline bool session_ok_to_route(DCB *dcb)
{
    bool rval = false;

    if (dcb->session->state == SESSION_STATE_ROUTER_READY &&
        dcb->session->client_dcb != NULL &&
        dcb->session->client_dcb->state == DCB_STATE_POLLING &&
        (dcb->session->router_session ||
         service_get_capabilities(dcb->session->service) & RCAP_TYPE_NO_RSESSION))
    {
        MySQLProtocol *client_protocol = (MySQLProtocol *)dcb->session->client_dcb->protocol;

        if (client_protocol)
        {
            CHK_PROTOCOL(client_protocol);

            if (client_protocol->protocol_auth_state == MXS_AUTH_STATE_COMPLETE)
            {
                rval = true;
            }
        }
        else if (dcb->session->client_dcb->dcb_role == DCB_ROLE_INTERNAL)
        {
            rval = true;
        }
    }

    return rval;
}

static inline bool expecting_resultset(MySQLProtocol *proto)
{
    return proto->current_command == MYSQL_COM_QUERY ||
           proto->current_command == MYSQL_COM_STMT_FETCH;
}

/**
 * Helpers for checking OK and ERR packets specific to COM_CHANGE_USER
 */
static inline bool not_ok_packet(const GWBUF* buffer)
{
    uint8_t* data = GWBUF_DATA(buffer);

    return data[4] != MYSQL_REPLY_OK ||
        // Should be more than 7 bytes of payload
        gw_mysql_get_byte3(data) < MYSQL_OK_PACKET_MIN_LEN - MYSQL_HEADER_LEN ||
        // Should have no affected rows
        data[5] != 0 ||
        // Should not generate an insert ID
        data[6] != 0;
}

static inline bool not_err_packet(const GWBUF* buffer)
{
    return GWBUF_DATA(buffer)[4] != MYSQL_REPLY_ERR;
}

/**
 * @brief With authentication completed, read new data and write to backend
 *
 * @param dcb           Descriptor control block for backend server
 * @param local_session Current MySQL session data structure
 * @return 0 is fail, 1 is success
 */
static int
gw_read_and_write(DCB *dcb)
{
    GWBUF *read_buffer = NULL;
    MXS_SESSION *session = dcb->session;
    int nbytes_read;
    int return_code = 0;

    CHK_SESSION(session);

    /* read available backend data */
    return_code = dcb_read(dcb, &read_buffer, 0);

    if (return_code < 0)
    {
        GWBUF* errbuf;
        bool succp;
#if defined(SS_DEBUG)
        MXS_ERROR("Backend read error handling #2.");
#endif
        errbuf = mysql_create_custom_error(1,
                                           0,
                                           "Read from backend failed");

        session->service->router->handleError(
            session->service->router_instance,
            session->router_session,
            errbuf,
            dcb,
            ERRACT_NEW_CONNECTION,
            &succp);
        gwbuf_free(errbuf);

        if (!succp)
        {
            session->state = SESSION_STATE_STOPPING;
        }
        return 0;
    }

    nbytes_read = gwbuf_length(read_buffer);
    if (nbytes_read == 0)
    {
        ss_dassert(read_buffer == NULL);
        return return_code;
    }
    else
    {
        ss_dassert(read_buffer != NULL);
    }

    /** Ask what type of output the router/filter chain expects */
    uint64_t capabilities = service_get_capabilities(session->service);
    MySQLProtocol *proto = (MySQLProtocol *)dcb->protocol;

    if (rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT) || proto->ignore_reply)
    {
        GWBUF *tmp = modutil_get_complete_packets(&read_buffer);
        /* Put any residue into the read queue */

        dcb->dcb_readqueue = read_buffer;

        if (tmp == NULL)
        {
            /** No complete packets */
            return 0;
        }

        read_buffer = tmp;

        if (rcap_type_required(capabilities, RCAP_TYPE_CONTIGUOUS_OUTPUT) || proto->ignore_reply)
        {
            if ((tmp = gwbuf_make_contiguous(read_buffer)))
            {
                read_buffer = tmp;
            }
            else
            {
                /** Failed to make the buffer contiguous */
                gwbuf_free(read_buffer);
                poll_fake_hangup_event(dcb);
                return 0;
            }

            if (rcap_type_required(capabilities, RCAP_TYPE_RESULTSET_OUTPUT) &&
                expecting_resultset(proto) && mxs_mysql_is_result_set(read_buffer))
            {
                int more = 0;
                if (modutil_count_signal_packets(read_buffer, 0, 0, &more) != 2)
                {
                    dcb->dcb_readqueue = read_buffer;
                    return 0;
                }
            }
        }
    }

    if (proto->ignore_reply)
    {
        /** The reply to a COM_CHANGE_USER is in packet */
        GWBUF *query = proto->stored_query;
        proto->stored_query = NULL;
        proto->ignore_reply = false;
        GWBUF* reply = modutil_get_next_MySQL_packet(&read_buffer);

        while (read_buffer)
        {
            /** Skip to the last packet if we get more than one */
            gwbuf_free(reply);
            reply = modutil_get_next_MySQL_packet(&read_buffer);
        }

        ss_dassert(reply);
        ss_dassert(!read_buffer);
        uint8_t result = MYSQL_GET_COMMAND(GWBUF_DATA(reply));
        int rval = 0;

        if (result == MYSQL_REPLY_OK)
        {
            MXS_INFO("Response to COM_CHANGE_USER is OK, writing stored query");
            rval = query ? dcb->func.write(dcb, query) : 1;
        }
        else if (result == MYSQL_REPLY_AUTHSWITCHREQUEST &&
                 gwbuf_length(reply) > MYSQL_EOF_PACKET_LEN)
        {
            /**
             * The server requested a change of authentication methods.
             * If we're changing the authentication method to the same one we
             * are using now, it means that the server is simply generating
             * a new scramble for the re-authentication process.
             */
            if (strcmp((char*)GWBUF_DATA(reply) + 5, DEFAULT_MYSQL_AUTH_PLUGIN) == 0)
            {
                /** Load the new scramble into the protocol... */
                gwbuf_copy_data(reply, 5 + strlen(DEFAULT_MYSQL_AUTH_PLUGIN) + 1,
                                GW_MYSQL_SCRAMBLE_SIZE, proto->scramble);

                /** ... and use it to send the encrypted password to the server */
                rval = send_mysql_native_password_response(dcb);

                /** Store the query until we know the result of the authentication
                 * method switch. */
                proto->stored_query = query;
                proto->ignore_reply = true;
                return rval;
            }
            else
            {
                /** The server requested a change to something other than
                 * the default auth plugin */
                gwbuf_free(query);
                poll_fake_hangup_event(dcb);

                // TODO: Use the authenticators to handle COM_CHANGE_USER responses
                MXS_ERROR("Received AuthSwitchRequest to '%s' when '%s' was expected",
                          (char*)GWBUF_DATA(reply) + 5, DEFAULT_MYSQL_AUTH_PLUGIN);

            }
        }
        else
        {
            if (result == MYSQL_REPLY_ERR)
            {
                /** The COM_CHANGE USER failed, generate a fake hangup event to
                 * close the DCB and send an error to the client. */
                handle_error_response(dcb, reply);
            }
            else
            {
                /** This should never happen */
                MXS_ERROR("Unknown response to COM_CHANGE_USER (0x%02hhx), "
                          "closing connection", result);
            }

            gwbuf_free(query);
            poll_fake_hangup_event(dcb);
        }

        gwbuf_free(reply);
        return rval;
    }

    do
    {
        GWBUF *stmt = NULL;
        /**
         * If protocol has session command set, concatenate whole
         * response into one buffer.
         */
        if (protocol_get_srv_command((MySQLProtocol *)dcb->protocol, false) != MYSQL_COM_UNDEFINED)
        {
            stmt = process_response_data(dcb, &read_buffer, gwbuf_length(read_buffer));
            /**
             * Received incomplete response to session command.
             * Store it to readqueue and return.
             */
            if (!sescmd_response_complete(dcb))
            {
                stmt = gwbuf_append(stmt, read_buffer);
                dcb->dcb_readqueue = gwbuf_append(stmt, dcb->dcb_readqueue);
                return 0;
            }

            if (!stmt)
            {
                MXS_ERROR("%lu [gw_read_backend_event] "
                          "Read buffer unexpectedly null, even though response "
                          "not marked as complete. User: %s",
                          pthread_self(), dcb->session->client_dcb->user);
                return 0;
            }
        }
        else if (rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT) &&
                 !rcap_type_required(capabilities, RCAP_TYPE_RESULTSET_OUTPUT))
        {
            stmt = modutil_get_next_MySQL_packet(&read_buffer);
        }
        else
        {
            stmt = read_buffer;
            read_buffer = NULL;
        }

        if (session_ok_to_route(dcb))
        {
            gwbuf_set_type(stmt, GWBUF_TYPE_MYSQL);
            session->service->router->clientReply(session->service->router_instance,
                                                  session->router_session,
                                                  stmt, dcb);
            return_code = 1;
        }
        else /*< session is closing; replying to client isn't possible */
        {
            gwbuf_free(stmt);
        }
    }
    while (read_buffer);

    return return_code;
}

/*
 * EPOLLOUT handler for the MySQL Backend protocol module.
 *
 * @param dcb   The descriptor control block
 * @return      1 in success, 0 in case of failure,
 */
static int gw_write_backend_event(DCB *dcb)
{
    int rc = 1;

    if (dcb->state != DCB_STATE_POLLING)
    {
        /** Don't write to backend if backend_dcb is not in poll set anymore */
        uint8_t* data = NULL;
        bool com_quit = false;

        if (dcb->writeq)
        {
            data = (uint8_t *) GWBUF_DATA(dcb->writeq);
            com_quit = MYSQL_IS_COM_QUIT(data);
        }

        if (data)
        {
            rc = 0;

            if (!com_quit)
            {
                mysql_send_custom_error(dcb->session->client_dcb, 1, 0,
                                        "Writing to backend failed due invalid Maxscale state.");
                MXS_ERROR("Attempt to write buffered data to backend "
                          "failed due internal inconsistent state: %s",
                          STRDCBSTATE(dcb->state));
            }
        }
        else
        {
            MXS_DEBUG("%lu [gw_write_backend_event] Dcb %p in state %s "
                      "but there's nothing to write either.",
                      pthread_self(), dcb, STRDCBSTATE(dcb->state));
        }
    }
    else
    {
        MySQLProtocol *backend_protocol = (MySQLProtocol*)dcb->protocol;

        if (backend_protocol->protocol_auth_state == MXS_AUTH_STATE_PENDING_CONNECT)
        {
            backend_protocol->protocol_auth_state = MXS_AUTH_STATE_CONNECTED;
        }
        else
        {
            dcb_drain_writeq(dcb);
        }

        MXS_DEBUG("%lu [gw_write_backend_event] wrote to dcb %p fd %d, return %d",
                  pthread_self(), dcb, dcb->fd, rc);
    }

    return rc;
}

/*
 * Write function for backend DCB. Store command to protocol.
 *
 * @param dcb   The DCB of the backend
 * @param queue Queue of buffers to write
 * @return      0 on failure, 1 on success
 */
static int gw_MySQLWrite_backend(DCB *dcb, GWBUF *queue)
{
    MySQLProtocol *backend_protocol = dcb->protocol;
    int rc = 0;

    CHK_DCB(dcb);

    if (dcb->was_persistent)
    {
        ss_dassert(!dcb->dcb_fakequeue);
        ss_dassert(!dcb->dcb_readqueue);
        ss_dassert(!dcb->delayq);
        ss_dassert(!dcb->writeq);
        ss_dassert(dcb->persistentstart == 0);
        dcb->was_persistent = false;
        backend_protocol->ignore_reply = false;

        if (dcb->state != DCB_STATE_POLLING ||
            backend_protocol->protocol_auth_state != MXS_AUTH_STATE_COMPLETE)
        {
            MXS_INFO("DCB and protocol state do not qualify for pooling: %s, %s",
                     STRDCBSTATE(dcb->state),
                     STRPROTOCOLSTATE(backend_protocol->protocol_auth_state));
            gwbuf_free(queue);
            return 0;
        }


        /**
         * This is a DCB that was just taken out of the persistent connection pool.
         * We need to sent a COM_CHANGE_USER query to the backend to reset the
         * session state.
         */
        if (backend_protocol->stored_query)
        {
            /** It is possible that the client DCB is closed before the COM_CHANGE_USER
             * response is received. */
            gwbuf_free(backend_protocol->stored_query);
        }

        if (MYSQL_IS_COM_QUIT(GWBUF_DATA(queue)))
        {
            /** The connection is being closed before the first write to this
             * backend was done. The COM_QUIT is ignored and the DCB will be put
             * back into the pool once it's closed. */
            MXS_INFO("COM_QUIT received as the first write, ignoring and "
                     "sending the DCB back to the pool.");
            gwbuf_free(queue);
            return 1;
        }

        GWBUF *buf = gw_create_change_user_packet(dcb->session->client_dcb->data, dcb->protocol);
        int rc = 0;

        if (dcb_write(dcb, buf))
        {
            MXS_INFO("Sent COM_CHANGE_USER");
            backend_protocol->ignore_reply = true;
            backend_protocol->stored_query = queue;
            rc = 1;
        }
        else
        {
            gwbuf_free(queue);
        }

        return rc;
    }
    else if (backend_protocol->ignore_reply)
    {
        if (MYSQL_IS_COM_QUIT((uint8_t*)GWBUF_DATA(queue)))
        {
            /** The COM_CHANGE_USER was already sent but the session is already
             * closing. */
            MXS_INFO("COM_QUIT received while COM_CHANGE_USER is in progress, closing pooled connection");
            gwbuf_free(queue);
            poll_fake_hangup_event(dcb);
            rc = 0;
        }
        else
        {
            /**
             * We're still waiting on the reply to the COM_CHANGE_USER, append the
             * buffer to the stored query. This is possible if the client sends
             * BLOB data on the first command or is sending multiple COM_QUERY
             * packets at one time.
             */
            MXS_INFO("COM_CHANGE_USER in progress, appending query to queue");
            backend_protocol->stored_query = gwbuf_append(backend_protocol->stored_query, queue);
            rc = 1;
        }
        return rc;
    }

    /**
     * Pick action according to state of protocol.
     * If auth failed, return value is 0, write and buffered write
     * return 1.
     */
    switch (backend_protocol->protocol_auth_state)
    {
    case MXS_AUTH_STATE_HANDSHAKE_FAILED:
    case MXS_AUTH_STATE_FAILED:
        if (dcb->session->state != SESSION_STATE_STOPPING)
        {
            MXS_ERROR("Unable to write to backend '%s' due to "
                      "%s failure. Server in state %s.",
                      dcb->server->unique_name,
                      backend_protocol->protocol_auth_state == MXS_AUTH_STATE_HANDSHAKE_FAILED ?
                      "handshake" : "authentication",
                      STRSRVSTATUS(dcb->server));
        }

        gwbuf_free(queue);
        rc = 0;

        break;

    case MXS_AUTH_STATE_COMPLETE:
        {
            uint8_t* ptr = GWBUF_DATA(queue);
            mysql_server_cmd_t cmd = MYSQL_GET_COMMAND(ptr);

            /** Copy the current command being executed to this backend */
            if (dcb->session->client_dcb && dcb->session->client_dcb->protocol)
            {
                MySQLProtocol *client_proto = (MySQLProtocol*)dcb->session->client_dcb->protocol;
                backend_protocol->current_command = client_proto->current_command;
            }

            MXS_DEBUG("%lu [gw_MySQLWrite_backend] write to dcb %p "
                      "fd %d protocol state %s.",
                      pthread_self(),
                      dcb,
                      dcb->fd,
                      STRPROTOCOLSTATE(backend_protocol->protocol_auth_state));


            /**
             * Statement type is used in readwrite split router.
             * Command is *not* set for readconn router.
             *
             * Server commands are stored to MySQLProtocol structure
             * if buffer always includes a single statement.
             */
            if (GWBUF_IS_TYPE_SINGLE_STMT(queue) &&
                GWBUF_IS_TYPE_SESCMD(queue))
            {
                /** Record the command to backend's protocol */
                protocol_add_srv_command(backend_protocol, cmd);
            }

            if (cmd == MYSQL_COM_QUIT && dcb->server->persistpoolmax)
            {
                /** We need to keep the pooled connections alive so we just ignore the COM_QUIT packet */
                gwbuf_free(queue);
                rc = 1;
            }
            else
            {
                /** Write to backend */
                rc = dcb_write(dcb, queue);
            }
        }
        break;

    default:
        {
            MXS_DEBUG("%lu [gw_MySQLWrite_backend] delayed write to "
                      "dcb %p fd %d protocol state %s.",
                      pthread_self(),
                      dcb,
                      dcb->fd,
                      STRPROTOCOLSTATE(backend_protocol->protocol_auth_state));
            /**
             * In case of session commands, store command to DCB's
             * protocol struct.
             */
            if (GWBUF_IS_TYPE_SINGLE_STMT(queue) &&
                GWBUF_IS_TYPE_SESCMD(queue))
            {
                uint8_t* ptr = GWBUF_DATA(queue);
                mysql_server_cmd_t cmd = MYSQL_GET_COMMAND(ptr);

                /** Record the command to backend's protocol */
                protocol_add_srv_command(backend_protocol, cmd);
            }
            /*<
             * Now put the incoming data to the delay queue unless backend is
             * connected with auth ok
             */
            backend_set_delayqueue(dcb, queue);

            rc = 1;
        }
        break;
    }
    return rc;
}

/**
 * Error event handler.
 * Create error message, pass it to router's error handler and if error
 * handler fails in providing enough backend servers, mark session being
 * closed and call DCB close function which triggers closing router session
 * and related backends (if any exists.
 */
static int gw_error_backend_event(DCB *dcb)
{
    MXS_SESSION* session;
    void* rsession;
    MXS_ROUTER_OBJECT* router;
    MXS_ROUTER* router_instance;
    GWBUF* errbuf;
    bool succp;
    mxs_session_state_t ses_state;

    CHK_DCB(dcb);
    session = dcb->session;
    CHK_SESSION(session);
    if (SESSION_STATE_DUMMY == session->state)
    {
        if (dcb->persistentstart == 0)
        {
            /** Not a persistent connection, something is wrong. */
            MXS_ERROR("EPOLLERR event on a non-persistent DCB with no session. "
                      "Closing connection.");
        }
        dcb_close(dcb);
        return 1;
    }
    rsession = session->router_session;
    router = session->service->router;
    router_instance = session->service->router_instance;

    /**
     * Avoid running redundant error handling procedure.
     * dcb_close is already called for the DCB. Thus, either connection is
     * closed by router and COM_QUIT sent or there was an error which
     * have already been handled.
     */
    if (dcb->state != DCB_STATE_POLLING)
    {
        int error, len;

        len = sizeof(error);

        if (getsockopt(dcb->fd, SOL_SOCKET, SO_ERROR, &error, (socklen_t *) & len) == 0)
        {
            if (error != 0)
            {
                char errstring[MXS_STRERROR_BUFLEN];
                MXS_ERROR("DCB in state %s got error '%s'.",
                          STRDCBSTATE(dcb->state),
                          strerror_r(error, errstring, sizeof(errstring)));
            }
        }
        return 1;
    }
    errbuf = mysql_create_custom_error(1,
                                       0,
                                       "Lost connection to backend server.");

    ses_state = session->state;

    if (ses_state != SESSION_STATE_ROUTER_READY)
    {
        int error, len;

        len = sizeof(error);
        if (getsockopt(dcb->fd, SOL_SOCKET, SO_ERROR, &error, (socklen_t *) & len) == 0)
        {
            if (error != 0)
            {
                char errstring[MXS_STRERROR_BUFLEN];
                MXS_ERROR("Error '%s' in session that is not ready for routing.",
                          strerror_r(error, errstring, sizeof(errstring)));
            }
        }
        gwbuf_free(errbuf);
        goto retblock;
    }

#if defined(SS_DEBUG)
    MXS_INFO("Backend error event handling.");
#endif
    router->handleError(router_instance,
                        rsession,
                        errbuf,
                        dcb,
                        ERRACT_NEW_CONNECTION,
                        &succp);
    gwbuf_free(errbuf);

    /**
     * If error handler fails it means that routing session can't continue
     * and it must be closed. In success, only this DCB is closed.
     */
    if (!succp)
    {
        session->state = SESSION_STATE_STOPPING;
    }

retblock:
    return 1;
}

/**
 * Error event handler.
 * Create error message, pass it to router's error handler and if error
 * handler fails in providing enough backend servers, mark session being
 * closed and call DCB close function which triggers closing router session
 * and related backends (if any exists.
 *
 * @param dcb The current Backend DCB
 * @return 1 always
 */
static int gw_backend_hangup(DCB *dcb)
{
    MXS_SESSION* session;
    void* rsession;
    MXS_ROUTER_OBJECT* router;
    MXS_ROUTER* router_instance;
    bool succp;
    GWBUF* errbuf;
    mxs_session_state_t ses_state;

    CHK_DCB(dcb);
    if (dcb->persistentstart)
    {
        dcb->dcb_errhandle_called = true;
        goto retblock;
    }
    session = dcb->session;

    if (session == NULL)
    {
        goto retblock;
    }

    CHK_SESSION(session);

    rsession = session->router_session;
    router = session->service->router;
    router_instance = session->service->router_instance;

    errbuf = mysql_create_custom_error(1,
                                       0,
                                       "Lost connection to backend server.");

    ses_state = session->state;

    if (ses_state != SESSION_STATE_ROUTER_READY)
    {
        int error, len;

        len = sizeof(error);
        if (getsockopt(dcb->fd, SOL_SOCKET, SO_ERROR, &error, (socklen_t *) & len) == 0)
        {
            if (error != 0 && ses_state != SESSION_STATE_STOPPING)
            {
                char errstring[MXS_STRERROR_BUFLEN];
                MXS_ERROR("Hangup in session that is not ready for routing, "
                          "Error reported is '%s'.",
                          strerror_r(error, errstring, sizeof(errstring)));
            }
        }
        gwbuf_free(errbuf);
        /*
         * I'm pretty certain this is best removed and
         * causes trouble if present, but have left it
         * here just for now as a comment. Martin
         */
        /* dcb_close(dcb); */
        goto retblock;
    }

    router->handleError(router_instance,
                        rsession,
                        errbuf,
                        dcb,
                        ERRACT_NEW_CONNECTION,
                        &succp);

    gwbuf_free(errbuf);
    /** There are no required backends available, close session. */
    if (!succp)
    {
        session->state = SESSION_STATE_STOPPING;
    }

retblock:
    return 1;
}

/**
 * Send COM_QUIT to backend so that it can be closed.
 * @param dcb The current Backend DCB
 * @return 1 always
 */
static int gw_backend_close(DCB *dcb)
{
    MXS_SESSION* session;
    GWBUF* quitbuf;

    CHK_DCB(dcb);
    session = dcb->session;

    MXS_DEBUG("%lu [gw_backend_close]", pthread_self());

    quitbuf = mysql_create_com_quit(NULL, 0);
    gwbuf_set_type(quitbuf, GWBUF_TYPE_MYSQL);

    /** Send COM_QUIT to the backend being closed */
    mysql_send_com_quit(dcb, 0, quitbuf);

    mysql_protocol_done(dcb);

    if (session)
    {
        CHK_SESSION(session);
        /**
         * The lock is needed only to protect the read of session->state and
         * session->client_dcb values. Client's state may change by other thread
         * but client's close and adding client's DCB to zombies list is executed
         * only if client's DCB's state does _not_ change in parallel.
         */

        /**
         * If session->state is STOPPING, start closing client session.
         * Otherwise only this backend connection is closed.
         */
        if (session->state == SESSION_STATE_STOPPING &&
            session->client_dcb != NULL)
        {
            if (session->client_dcb->state == DCB_STATE_POLLING)
            {
                /** Close client DCB */
                dcb_close(session->client_dcb);
            }
        }
    }
    return 1;
}

/**
 * This routine put into the delay queue the input queue
 * The input is what backend DCB is receiving
 * The routine is called from func.write() when mysql backend connection
 * is not yet complete buu there are inout data from client
 *
 * @param dcb   The current backend DCB
 * @param queue Input data in the GWBUF struct
 */
static void backend_set_delayqueue(DCB *dcb, GWBUF *queue)
{
    /* Append data */
    dcb->delayq = gwbuf_append(dcb->delayq, queue);
}

/**
 * This routine writes the delayq via dcb_write
 * The dcb->delayq contains data received from the client before
 * mysql backend authentication succeded
 *
 * @param dcb The current backend DCB
 * @return The dcb_write status
 */
static int backend_write_delayqueue(DCB *dcb, GWBUF *buffer)
{
    ss_dassert(buffer);
    ss_dassert(dcb->persistentstart == 0);
    ss_dassert(!dcb->was_persistent);

    if (MYSQL_IS_CHANGE_USER(((uint8_t *)GWBUF_DATA(buffer))))
    {
        /** Recreate the COM_CHANGE_USER packet with the scramble the backend sent to us */
        MYSQL_session mses;
        gw_get_shared_session_auth_info(dcb, &mses);
        gwbuf_free(buffer);
        buffer = gw_create_change_user_packet(&mses, dcb->protocol);
    }

    int rc = 1;

    if (MYSQL_IS_COM_QUIT(((uint8_t*)GWBUF_DATA(buffer))) && dcb->server->persistpoolmax)
    {
        /** We need to keep the pooled connections alive so we just ignore the COM_QUIT packet */
        gwbuf_free(buffer);
        rc = 1;
    }
    else
    {
        rc = dcb_write(dcb, buffer);
    }

    if (rc == 0)
    {
        MXS_SESSION *session = dcb->session;
        CHK_SESSION(session);
        MXS_ROUTER_OBJECT *router = session->service->router;
        MXS_ROUTER *router_instance = session->service->router_instance;
        void *rsession = session->router_session;
        bool succp = false;
        GWBUF* errbuf = mysql_create_custom_error(
                            1, 0, "Failed to write buffered data to back-end server. "
                            "Buffer was empty or back-end was disconnected during "
                            "operation. Attempting to find a new backend.");

        router->handleError(router_instance,
                            rsession,
                            errbuf,
                            dcb,
                            ERRACT_NEW_CONNECTION,
                            &succp);
        gwbuf_free(errbuf);

        if (!succp)
        {
            session->state = SESSION_STATE_STOPPING;
        }
    }

    return rc;
}

/**
 * This routine handles the COM_CHANGE_USER command
 *
 * TODO: Move this into the authenticators
 *
 * @param dcb           The current backend DCB
 * @param server        The backend server pointer
 * @param in_session    The current session data (MYSQL_session)
 * @param queue         The GWBUF containing the COM_CHANGE_USER receveid
 * @return 1 on success and 0 on failure
 */
static int gw_change_user(DCB *backend,
                          SERVER *server,
                          MXS_SESSION *in_session,
                          GWBUF *queue)
{
    MYSQL_session *current_session = NULL;
    MySQLProtocol *backend_protocol = NULL;
    MySQLProtocol *client_protocol = NULL;
    char username[MYSQL_USER_MAXLEN + 1] = "";
    char database[MYSQL_DATABASE_MAXLEN + 1] = "";
    char current_database[MYSQL_DATABASE_MAXLEN + 1] = "";
    uint8_t client_sha1[MYSQL_SCRAMBLE_LEN] = "";
    uint8_t *client_auth_packet = GWBUF_DATA(queue);
    unsigned int auth_token_len = 0;
    uint8_t *auth_token = NULL;
    int rv = -1;
    int auth_ret = 1;

    current_session = (MYSQL_session *)in_session->client_dcb->data;
    backend_protocol = backend->protocol;
    client_protocol = in_session->client_dcb->protocol;

    /* now get the user, after 4 bytes header and 1 byte command */
    client_auth_packet += 5;
    size_t len = strlen((char *)client_auth_packet);
    if (len > MYSQL_USER_MAXLEN)
    {
        MXS_ERROR("Client sent user name \"%s\",which is %lu characters long, "
                  "while a maximum length of %d is allowed. Cutting trailing "
                  "characters.", (char*)client_auth_packet, len, MYSQL_USER_MAXLEN);
    }
    strncpy(username, (char *)client_auth_packet, MYSQL_USER_MAXLEN);
    username[MYSQL_USER_MAXLEN] = 0;

    client_auth_packet += (len + 1);

    /* get the auth token len */
    memcpy(&auth_token_len, client_auth_packet, 1);

    client_auth_packet++;

    /* allocate memory for token only if auth_token_len > 0 */
    if (auth_token_len > 0)
    {
        auth_token = (uint8_t *)MXS_MALLOC(auth_token_len);
        ss_dassert(auth_token != NULL);

        if (auth_token == NULL)
        {
            return rv;
        }
        memcpy(auth_token, client_auth_packet, auth_token_len);
        client_auth_packet += auth_token_len;
    }

    /* get new database name */
    len = strlen((char *)client_auth_packet);
    if (len > MYSQL_DATABASE_MAXLEN)
    {
        MXS_ERROR("Client sent database name \"%s\", which is %lu characters long, "
                  "while a maximum length of %d is allowed. Cutting trailing "
                  "characters.", (char*)client_auth_packet, len, MYSQL_DATABASE_MAXLEN);
    }
    strncpy(database, (char *)client_auth_packet, MYSQL_DATABASE_MAXLEN);
    database[MYSQL_DATABASE_MAXLEN] = 0;

    client_auth_packet += (len + 1);

    if (*client_auth_packet)
    {
        memcpy(&backend_protocol->charset, client_auth_packet, sizeof(int));
    }

    /* save current_database name */
    strcpy(current_database, current_session->db);

    /*
     * Now clear database name in dcb as we don't do local authentication on db name for change user.
     * Local authentication only for user@host and if successful the database name change is sent to backend.
     */
    *current_session->db = 0;

    /*
     * Decode the token and check the password.
     * Note: if auth_token_len == 0 && auth_token == NULL, user is without password
     */
    DCB *dcb = backend->session->client_dcb;

    if (dcb->authfunc.reauthenticate == NULL)
    {
        /** Authenticator does not support reauthentication */
        rv = 0;
        goto retblock;
    }

    auth_ret = dcb->authfunc.reauthenticate(dcb, username,
                                            auth_token, auth_token_len,
                                            client_protocol->scramble,
                                            sizeof(client_protocol->scramble),
                                            client_sha1, sizeof(client_sha1));

    strcpy(current_session->db, current_database);

    if (auth_ret != 0)
    {
        if (service_refresh_users(backend->session->client_dcb->service) == 0)
        {
            /* Try authentication again with new repository data */
            /* Note: if no auth client authentication will fail */
            *current_session->db = 0;

            auth_ret = dcb->authfunc.reauthenticate(dcb, username,
                                                    auth_token, auth_token_len,
                                                    client_protocol->scramble,
                                                    sizeof(client_protocol->scramble),
                                                    client_sha1, sizeof(client_sha1));

            strcpy(current_session->db, current_database);
        }
    }

    MXS_FREE(auth_token);

    if (auth_ret != 0)
    {
        char *password_set = NULL;
        char *message = NULL;

        if (auth_token_len > 0)
        {
            password_set = (char *)client_sha1;
        }
        else
        {
            password_set = "";
        }

        /**
         * Create an error message and make it look like legit reply
         * from backend server. Then make it look like an incoming event
         * so that thread gets new task of it, calls clientReply
         * which filters out duplicate errors from same cause and forward
         * reply to the client.
         */
        message = create_auth_fail_str(username,
                                       backend->session->client_dcb->remote,
                                       password_set,
                                       false,
                                       auth_ret);
        if (message == NULL)
        {
            MXS_ERROR("Creating error message failed.");
            rv = 0;
            goto retblock;
        }
        /**
         * Add command to backend's protocol, create artificial reply
         * packet and add it to client's read buffer.
         */
        protocol_add_srv_command((MySQLProtocol*)backend->protocol,
                                 MYSQL_COM_CHANGE_USER);
        modutil_reply_auth_error(backend, message, 0);
        rv = 1;
    }
    else
    {
        /** This assumes that authentication will succeed. If authentication fails,
         * the internal session will represent the wrong user. This is wrong and
         * a check whether the COM_CHANGE_USER succeeded should be done in the
         * backend protocol reply handling.
         *
         * For the time being, it is simpler to assume a COM_CHANGE_USER will always
         * succeed if the authentication in MaxScale is successful. In practice this
         * might not be true but these cases are handled by the router modules
         * and the servers that fail to execute the COM_CHANGE_USER are discarded. */
        strcpy(current_session->user, username);
        strcpy(current_session->db, database);
        memcpy(current_session->client_sha1, client_sha1, sizeof(current_session->client_sha1));
        rv = gw_send_change_user_to_backend(database, username, client_sha1, backend_protocol);
    }

retblock:
    gwbuf_free(queue);

    return rv;
}

/**
 * Move packets or parts of packets from readbuf to outbuf as the packet headers
 * and lengths have been noticed and counted.
 * Session commands need to be marked so that they can be handled properly in
 * the router's clientReply.
 *
 * @param dcb                   Backend's DCB where data was read from
 * @param readbuf               GWBUF where data was read to
 * @param nbytes_to_process     Number of bytes that has been read and need to be processed
 *
 * @return GWBUF which includes complete MySQL packet
 */
static GWBUF* process_response_data(DCB* dcb,
                                    GWBUF** readbuf,
                                    int nbytes_to_process)
{
    int npackets_left = 0; /*< response's packet count */
    ssize_t nbytes_left = 0; /*< nbytes to be read for the packet */
    MySQLProtocol* p;
    GWBUF* outbuf = NULL;
    int initial_packets = npackets_left;
    ssize_t initial_bytes = nbytes_left;

    /** Get command which was stored in gw_MySQLWrite_backend */
    p = DCB_PROTOCOL(dcb, MySQLProtocol);
    if (!DCB_IS_CLONE(dcb))
    {
        CHK_PROTOCOL(p);
    }

    /** All buffers processed here are sescmd responses */
    gwbuf_set_type(*readbuf, GWBUF_TYPE_SESCMD_RESPONSE);

    /**
     * Now it is known how many packets there should be and how much
     * is read earlier.
     */
    while (nbytes_to_process != 0)
    {
        mysql_server_cmd_t srvcmd;
        bool succp;

        srvcmd = protocol_get_srv_command(p, false);

        MXS_DEBUG("%lu [process_response_data] Read command %s for DCB %p fd %d.",
                  pthread_self(),
                  STRPACKETTYPE(srvcmd),
                  dcb,
                  dcb->fd);
        /**
         * Read values from protocol structure, fails if values are
         * uninitialized.
         */
        if (npackets_left == 0)
        {
            succp = protocol_get_response_status(p, &npackets_left, &nbytes_left);

            if (!succp || npackets_left == 0)
            {
                /**
                 * Examine command type and the readbuf. Conclude response
                 * packet count from the command type or from the first
                 * packet content. Fails if read buffer doesn't include
                 * enough data to read the packet length.
                 */
                init_response_status(*readbuf, srvcmd, &npackets_left, &nbytes_left);
            }

            initial_packets = npackets_left;
            initial_bytes = nbytes_left;
        }
        /** Only session commands with responses should be processed */
        ss_dassert(npackets_left > 0);

        /** Read incomplete packet. */
        if (nbytes_left > nbytes_to_process)
        {
            /** Includes length info so it can be processed */
            if (nbytes_to_process >= 5)
            {
                /** discard source buffer */
                *readbuf = gwbuf_consume(*readbuf, GWBUF_LENGTH(*readbuf));
                nbytes_left -= nbytes_to_process;
            }
            nbytes_to_process = 0;
        }
        /** Packet was read. All bytes belonged to the last packet. */
        else if (nbytes_left == nbytes_to_process)
        {
            nbytes_left = 0;
            nbytes_to_process = 0;
            ss_dassert(npackets_left > 0);
            npackets_left -= 1;
            outbuf = gwbuf_append(outbuf, *readbuf);
            *readbuf = NULL;
        }
        /**
         * Buffer contains more data than we need. Split the complete packet and
         * the extra data into two separate buffers.
         */
        else
        {
            ss_dassert(nbytes_left < nbytes_to_process);
            ss_dassert(nbytes_left > 0);
            ss_dassert(npackets_left > 0);
            outbuf = gwbuf_append(outbuf, gwbuf_split(readbuf, nbytes_left));
            nbytes_to_process -= nbytes_left;
            npackets_left -= 1;
            nbytes_left = 0;
        }

        /** Store new status to protocol structure */
        protocol_set_response_status(p, npackets_left, nbytes_left);

        /** A complete packet was read */
        if (nbytes_left == 0)
        {
            /** No more packets in this response */
            if (npackets_left == 0 && outbuf != NULL)
            {
                GWBUF* b = outbuf;

                while (b->next != NULL)
                {
                    b = b->next;
                }
                /** Mark last as end of response */
                gwbuf_set_type(b, GWBUF_TYPE_RESPONSE_END);

                /** Archive the command */
                protocol_archive_srv_command(p);

                /** Ignore the rest of the response */
                nbytes_to_process = 0;
            }
            /** Read next packet */
            else
            {
                uint8_t* data;

                /** Read next packet length if there is at least
                 * three bytes left. If there is less than three
                 * bytes in the buffer or it is NULL, we need to
                 wait for more data from the backend server.*/
                if (*readbuf == NULL || gwbuf_length(*readbuf) < 3)
                {
                    MXS_DEBUG("%lu [%s] Read %d packets. Waiting for %d more "
                              "packets for a total of %d packets.",
                              pthread_self(), __FUNCTION__,
                              initial_packets - npackets_left,
                              npackets_left, initial_packets);

                    /** Store the already read data into the readqueue of the DCB
                     * and restore the response status to the initial number of packets */

                    dcb->dcb_readqueue = gwbuf_append(outbuf, dcb->dcb_readqueue);

                    protocol_set_response_status(p, initial_packets, initial_bytes);
                    return NULL;
                }
                uint8_t packet_len[3];
                gwbuf_copy_data(*readbuf, 0, 3, packet_len);
                nbytes_left = gw_mysql_get_byte3(packet_len) + MYSQL_HEADER_LEN;
                /** Store new status to protocol structure */
                protocol_set_response_status(p, npackets_left, nbytes_left);
            }
        }
    }
    return outbuf;
}

static bool sescmd_response_complete(DCB* dcb)
{
    int npackets_left;
    ssize_t nbytes_left;
    MySQLProtocol* p;
    bool succp;

    p = DCB_PROTOCOL(dcb, MySQLProtocol);
    if (!DCB_IS_CLONE(dcb))
    {
        CHK_PROTOCOL(p);
    }

    protocol_get_response_status(p, &npackets_left, &nbytes_left);

    if (npackets_left == 0)
    {
        succp = true;
    }
    else
    {
        succp = false;
    }
    return succp;
}

/**
 * Create COM_CHANGE_USER packet and store it to GWBUF
 *
 * @param mses          MySQL session
 * @param protocol      protocol structure of the backend
 *
 * @return GWBUF buffer consisting of COM_CHANGE_USER packet
 *
 * @note the function doesn't fail
 */
static GWBUF *
gw_create_change_user_packet(MYSQL_session*  mses,
                             MySQLProtocol*  protocol)
{
    char* db;
    char* user;
    uint8_t* pwd;
    GWBUF* buffer;
    int compress = 0;
    uint8_t* payload = NULL;
    uint8_t* payload_start = NULL;
    long bytes;
    char dbpass[MYSQL_USER_MAXLEN + 1] = "";
    char* curr_db = NULL;
    uint8_t* curr_passwd = NULL;
    unsigned int charset;

    db = mses->db;
    user = mses->user;
    pwd = mses->client_sha1;

    if (strlen(db) > 0)
    {
        curr_db = db;
    }

    if (memcmp(pwd, null_client_sha1, MYSQL_SCRAMBLE_LEN))
    {
        curr_passwd = pwd;
    }

    /* get charset the client sent and use it for connection auth */
    charset = protocol->charset;

    if (compress)
    {
#ifdef DEBUG_MYSQL_CONN
        fprintf(stderr, ">>>> Backend Connection with compression\n");
#endif
    }

    /**
     * Protocol MySQL COM_CHANGE_USER for CLIENT_PROTOCOL_41
     * 1 byte COMMAND
     */
    bytes = 1;

    /** add the user and a terminating char */
    bytes += strlen(user);
    bytes++;
    /**
     * next will be + 1 (scramble_len) + 20 (fixed_scramble) +
     * (db + NULL term) + 2 bytes charset
     */
    if (curr_passwd != NULL)
    {
        bytes += GW_MYSQL_SCRAMBLE_SIZE;
    }
    /** 1 byte for scramble_len */
    bytes++;
    /** db name and terminating char */
    if (curr_db != NULL)
    {
        bytes += strlen(curr_db);
    }
    bytes++;

    /** the charset */
    bytes += 2;
    bytes += strlen("mysql_native_password");
    bytes++;

    /** the packet header */
    bytes += 4;

    buffer = gwbuf_alloc(bytes);
    /**
     * Set correct type to GWBUF so that it will be handled like session
     * commands
     */
    buffer->gwbuf_type = GWBUF_TYPE_MYSQL | GWBUF_TYPE_SINGLE_STMT | GWBUF_TYPE_SESCMD;
    payload = GWBUF_DATA(buffer);
    memset(payload, '\0', bytes);
    payload_start = payload;

    /** set packet number to 0 */
    payload[3] = 0x00;
    payload += 4;

    /** set the command COM_CHANGE_USER 0x11 */
    payload[0] = 0x11;
    payload++;
    memcpy(payload, user, strlen(user));
    payload += strlen(user);
    payload++;

    if (curr_passwd != NULL)
    {
        uint8_t hash1[GW_MYSQL_SCRAMBLE_SIZE] = "";
        uint8_t hash2[GW_MYSQL_SCRAMBLE_SIZE] = "";
        uint8_t new_sha[GW_MYSQL_SCRAMBLE_SIZE] = "";
        uint8_t client_scramble[GW_MYSQL_SCRAMBLE_SIZE];

        /** hash1 is the function input, SHA1(real_password) */
        memcpy(hash1, pwd, GW_MYSQL_SCRAMBLE_SIZE);

        /**
         * hash2 is the SHA1(input data), where
         * input_data = SHA1(real_password)
         */
        gw_sha1_str(hash1, GW_MYSQL_SCRAMBLE_SIZE, hash2);

        /** dbpass is the HEX form of SHA1(SHA1(real_password)) */
        gw_bin2hex(dbpass, hash2, GW_MYSQL_SCRAMBLE_SIZE);

        /** new_sha is the SHA1(CONCAT(scramble, hash2) */
        gw_sha1_2_str(protocol->scramble,
                      GW_MYSQL_SCRAMBLE_SIZE,
                      hash2,
                      GW_MYSQL_SCRAMBLE_SIZE,
                      new_sha);

        /** compute the xor in client_scramble */
        gw_str_xor(client_scramble,
                   new_sha, hash1,
                   GW_MYSQL_SCRAMBLE_SIZE);

        /** set the auth-length */
        *payload = GW_MYSQL_SCRAMBLE_SIZE;
        payload++;
        /**
         * copy the 20 bytes scramble data after
         * packet_buffer + 36 + user + NULL + 1 (byte of auth-length)
         */
        memcpy(payload, client_scramble, GW_MYSQL_SCRAMBLE_SIZE);
        payload += GW_MYSQL_SCRAMBLE_SIZE;
    }
    else
    {
        /** skip the auth-length and leave the byte as NULL */
        payload++;
    }
    /** if the db is not NULL append it */
    if (curr_db != NULL)
    {
        memcpy(payload, curr_db, strlen(curr_db));
        payload += strlen(curr_db);
    }
    payload++;
    /** set the charset, 2 bytes */
    *payload = charset;
    payload++;
    *payload = '\x00';
    payload++;
    memcpy(payload, "mysql_native_password", strlen("mysql_native_password"));
    /* Following needed if more to be added */
    /* payload += strlen("mysql_native_password"); */
    /** put here the paylod size: bytes to write - 4 bytes packet header */
    gw_mysql_set_byte3(payload_start, (bytes - 4));

    return buffer;
}

/**
 * Write a MySQL CHANGE_USER packet to backend server
 *
 * @param conn  MySQL protocol structure
 * @param dbname The selected database
 * @param user The selected user
 * @param passwd The SHA1(real_password)
 * @return 1 on success, 0 on failure
 */
static int
gw_send_change_user_to_backend(char          *dbname,
                               char          *user,
                               uint8_t       *passwd,
                               MySQLProtocol *conn)
{
    GWBUF *buffer;
    int rc;
    MYSQL_session*  mses;

    mses = (MYSQL_session*)conn->owner_dcb->session->client_dcb->data;
    buffer = gw_create_change_user_packet(mses, conn);
    rc = conn->owner_dcb->func.write(conn->owner_dcb, buffer);

    if (rc != 0)
    {
        rc = 1;
    }
    return rc;
}

static bool gw_connection_established(DCB* dcb)
{
    MySQLProtocol *proto = (MySQLProtocol*)dcb->protocol;
    return proto->protocol_auth_state == MXS_AUTH_STATE_COMPLETE &&
           !proto->ignore_reply && !proto->stored_query;
}

#include "wolfssl_server.h"
#include "buffer_pool.h"
#include "managers/socket_manager.h"
#include "loggers/network_logger.h"
#include "utils/jsonutils.h"

#include <wolfssl/options.h>
#include <wolfssl/openssl/bio.h>
#include <wolfssl/openssl/err.h>
#include <wolfssl/openssl/pem.h>
#include <wolfssl/openssl/ssl.h>

#define STATE(x) ((wssl_server_state_t *)((x)->state))
#define CSTATE(x) ((wssl_server_con_state_t *)((((x)->line->chains_state)[self->chain_index])))
#define CSTATE_MUT(x) ((x)->line->chains_state)[self->chain_index]
#define ISALIVE(x) (CSTATE(x) != NULL)

typedef struct wssl_server_state_s
{

    SSL_CTX *ssl_context;
    char *alpns;
    // settings

} wssl_server_state_t;

typedef struct wssl_server_con_state_s
{

    bool handshake_completed;
    SSL *ssl;

    BIO *rbio;
    BIO *wbio;
    int fd;

    bool first_sent;
    bool init_sent;

} wssl_server_con_state_t;

static int on_alpn_select(SSL *ssl,
                          const unsigned char **out,
                          unsigned char *outlen,
                          const unsigned char *in,
                          unsigned int inlen,
                          void *arg)
{
    if (inlen == 0)
    {
        return SSL_TLSEXT_ERR_NOACK;
    }

    unsigned int offset = 0;
    while (offset < inlen)
    {
        LOGD("client ALPN ->  %.*s", in[offset], &(in[1 + offset]));
        offset = offset + 1 + in[offset];

        // TODO alpn paths
    }
    // selecting first alpn -_-
    *out = in + 1;
    *outlen = in[0];
    return SSL_TLSEXT_ERR_OK;
    // return SSL_TLSEXT_ERR_NOACK;
}

enum sslstatus
{
    SSLSTATUS_OK,
    SSLSTATUS_WANT_IO,
    SSLSTATUS_FAIL
};

static enum sslstatus get_sslstatus(SSL *ssl, int n)
{
    switch (SSL_get_error(ssl, n))
    {
    case SSL_ERROR_NONE:
        return SSLSTATUS_OK;
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_READ:
        return SSLSTATUS_WANT_IO;
    case SSL_ERROR_ZERO_RETURN:
    case SSL_ERROR_SYSCALL:
    default:
        return SSLSTATUS_FAIL;
    }
}

static void cleanup(tunnel_t *self, context_t *c)
{
    if (CSTATE(c) != NULL)
    {
        SSL_free(CSTATE(c)->ssl); /* free the SSL object and its BIO's */
        free(CSTATE(c));
        CSTATE_MUT(c) = NULL;
    }
}

static inline void upStream(tunnel_t *self, context_t *c)
{
    wssl_server_state_t *state = STATE(self);

    if (c->payload != NULL)
    {
        wssl_server_con_state_t *cstate = CSTATE(c);
        enum sslstatus status;
        int n;
        size_t len = bufLen(c->payload);

        while (len > 0)
        {
            n = BIO_write(cstate->rbio, rawBuf(c->payload), len);

            if (n <= 0)
            {
                /* if BIO write fails, assume unrecoverable */
                DISCARD_CONTEXT(c);
                goto failed;
            }
            shiftr(c->payload, n);
            len -= n;

            if (!SSL_is_init_finished(cstate->ssl))
            {
                n = SSL_accept(cstate->ssl);
                status = get_sslstatus(cstate->ssl, n);

                /* Did SSL request to write bytes? */
                if (status == SSLSTATUS_WANT_IO)
                    do
                    {
                        shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
                        size_t avail = rCap(buf);
                        n = BIO_read(cstate->wbio, rawBuf(buf), avail);
                        // assert(-1 == BIO_read(cstate->wbio, rawBuf(buf), avail));
                        if (n > 0)
                        {
                            setLen(buf, n);
                            context_t *answer = newContext(c->line);
                            answer->payload = buf;
                            self->dw->downStream(self->dw, answer);
                            if (!ISALIVE(c))
                            {
                                DISCARD_CONTEXT(c);
                                destroyContext(c);
                                return;
                            }
                        }
                        else if (!BIO_should_retry(cstate->wbio))
                        {
                            // If BIO_should_retry() is false then the cause is an error condition.
                            DISCARD_CONTEXT(c);
                            reuseBuffer(buffer_pools[c->line->tid], buf);
                            goto failed;
                        }
                        else
                        {
                            reuseBuffer(buffer_pools[c->line->tid], buf);
                        }
                    } while (n > 0);

                if (status == SSLSTATUS_FAIL)
                {
                    DISCARD_CONTEXT(c);
                    goto failed;
                }

                if (!SSL_is_init_finished(cstate->ssl))
                {
                    DISCARD_CONTEXT(c);
                    destroyContext(c);
                    return;
                }
                else
                {
                    LOGD("Tls handshake complete");
                    cstate->handshake_completed = true;
                    context_t *up_init_ctx = newContext(c->line);
                    up_init_ctx->init = true;
                    up_init_ctx->src_io = c->src_io;

                    self->up->upStream(self->up, up_init_ctx);
                    if (!ISALIVE(c))
                    {
                        LOGW("Openssl server: next node instantly closed the init with fin");
                        DISCARD_CONTEXT(c);
                        destroyContext(c);

                        return;
                    }
                    cstate->init_sent = true;
                }
            }

            /* The encrypted data is now in the input bio so now we can perform actual
             * read of unencrypted data. */
            shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
            shiftl(buf, 8192 / 2);
            setLen(buf, 0);
            do
            {
                size_t avail = rCap(buf) - bufLen(buf);
                n = SSL_read(cstate->ssl, rawBuf(buf) + bufLen(buf), avail);
                if (n > 0)
                {
                    setLen(buf, bufLen(buf) + n);
                }

            } while (n > 0);

            if (bufLen(buf) > 0)
            {
                context_t *up_ctx = newContext(c->line);
                up_ctx->payload = buf;
                up_ctx->src_io = c->src_io;
                if (!(cstate->first_sent))
                {
                    up_ctx->first = true;
                    cstate->first_sent = true;
                }
                self->up->upStream(self->up, up_ctx);
                if (!ISALIVE(c))
                {
                    DISCARD_CONTEXT(c);
                    destroyContext(c);
                    return;
                }
            }
            else
            {
                reuseBuffer(buffer_pools[c->line->tid], buf);
            }

            status = get_sslstatus(cstate->ssl, n);

            /* Did SSL request to write bytes? This can happen if peer has requested SSL
             * renegotiation. */
            if (status == SSLSTATUS_WANT_IO)
                do
                {
                    shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
                    size_t avail = rCap(buf);

                    n = BIO_read(cstate->wbio, rawBuf(buf), avail);
                    if (n > 0)
                    {
                        setLen(buf, n);
                        context_t *answer = newContext(c->line);
                        answer->payload = buf;
                        self->dw->downStream(self->dw, answer);
                        if (!ISALIVE(c))
                        {
                            DISCARD_CONTEXT(c);
                            destroyContext(c);

                            return;
                        }
                    }
                    else if (!BIO_should_retry(cstate->wbio))
                    {
                        // If BIO_should_retry() is false then the cause is an error condition.
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                        DISCARD_CONTEXT(c);
                        destroyContext(c);

                        goto failed_after_establishment;
                    }
                    else
                    {
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                    }
                } while (n > 0);

            if (status == SSLSTATUS_FAIL)
            {
                DISCARD_CONTEXT(c);
                goto failed_after_establishment;
            }
        }
        // done with socket data
        DISCARD_CONTEXT(c);
        destroyContext(c);
    }
    else
    {

        if (c->init)
        {
            CSTATE_MUT(c) = malloc(sizeof(wssl_server_con_state_t));
            memset(CSTATE(c), 0, sizeof(wssl_server_con_state_t));
            wssl_server_con_state_t *cstate = CSTATE(c);
            cstate->fd = hio_fd(c->src_io);
            cstate->rbio = BIO_new(BIO_s_mem());
            cstate->wbio = BIO_new(BIO_s_mem());

            cstate->ssl = SSL_new(state->ssl_context);
            SSL_set_accept_state(cstate->ssl); /* sets ssl to work in server mode. */
            SSL_set_bio(cstate->ssl, cstate->rbio, cstate->wbio);
            destroyContext(c);
        }
        else if (c->fin)
        {
            if (CSTATE(c)->init_sent)
            {
                cleanup(self, c);
                self->up->upStream(self->up, c);
            }
            else
            {
                cleanup(self, c);
                destroyLine(c->line);
                destroyContext(c);
            }
        }
    }

    return;

failed_after_establishment:
    context_t *fail_context_up = newContext(c->line);
    fail_context_up->fin = true;
    fail_context_up->src_io = c->src_io;
    self->up->upStream(self->up, fail_context_up);
failed:
    context_t *fail_context = newContext(c->line);
    fail_context->fin = true;
    fail_context->src_io = NULL;
    cleanup(self, c);
    destroyContext(c);
    self->dw->downStream(self->dw, fail_context);
    return;
}

static inline void downStream(tunnel_t *self, context_t *c)
{
    wssl_server_con_state_t *cstate = CSTATE(c);
    if (c->payload != NULL)
    {
        // self->dw->downStream(self->dw, ctx);
        // char buf[DEFAULT_BUF_SIZE];
        enum sslstatus status;

        if (!SSL_is_init_finished(cstate->ssl))
        {
            LOGF("How it is possilbe to receive data before sending init to upstream?");
            exit(1);
        }
        size_t len = bufLen(c->payload);
        while (len)
        {
            int n = SSL_write(cstate->ssl, rawBuf(c->payload), len);
            status = get_sslstatus(cstate->ssl, n);

            if (n > 0)
            {
                /* consume the waiting bytes that have been used by SSL */
                shiftr(c->payload, n);
                len -= n;
                /* take the output of the SSL object and queue it for socket write */
                do
                {

                    shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
                    size_t avail = rCap(buf);
                    n = BIO_read(cstate->wbio, rawBuf(buf), avail);
                    if (n > 0)
                    {
                        setLen(buf, n);
                        context_t *dw_context = newContext(c->line);
                        dw_context->payload = buf;
                        dw_context->src_io = c->src_io;
                        self->dw->downStream(self->dw, dw_context);
                        if (!ISALIVE(c))
                        {
                            DISCARD_CONTEXT(c);
                            destroyContext(c);

                            return;
                        }
                    }
                    else if (!BIO_should_retry(cstate->wbio))
                    {
                        // If BIO_should_retry() is false then the cause is an error condition.
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                        DISCARD_CONTEXT(c);
                        goto failed_after_establishment;
                    }
                    else
                    {
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                    }
                } while (n > 0);
            }

            if (status == SSLSTATUS_FAIL)
            {
                DISCARD_CONTEXT(c);
                goto failed_after_establishment;
            }

            if (n == 0)
                break;
        }
        assert(bufLen(c->payload) == 0);
        DISCARD_CONTEXT(c);
        destroyContext(c);

        return;
    }
    else
    {
        if (c->est)
        {
            self->dw->downStream(self->dw, c);
            return;
        }
        else if (c->fin)
        {
            cleanup(self, c);
            self->dw->downStream(self->dw, c);
        }
    }
    return;

failed_after_establishment:
    context_t *fail_context_up = newContext(c->line);
    fail_context_up->fin = true;
    fail_context_up->src_io = NULL;
    self->up->upStream(self->up, fail_context_up);

    context_t *fail_context = newContext(c->line);
    fail_context->fin = true;
    fail_context->src_io = c->src_io;
    cleanup(self, c);
    destroyContext(c);
    self->dw->downStream(self->dw, fail_context);

    return;
}

static void openSSLUpStream(tunnel_t *self, context_t *c)
{
    upStream(self, c);
}
static void openSSLPacketUpStream(tunnel_t *self, context_t *c)
{
    upStream(self, c);
}
static void openSSLDownStream(tunnel_t *self, context_t *c)
{
    downStream(self, c);
}
static void openSSLPacketDownStream(tunnel_t *self, context_t *c)
{
    downStream(self, c);
}
typedef struct
{
    const char *crt_file;
    const char *key_file;
    const char *ca_file;
    const char *ca_path;
    short verify_peer;
    short endpoint;
} ssl_ctx_opt_t;

SSL_CTX* ssl_ctx_new(ssl_ctx_opt_t* param) {
    static int s_initialized = 0;
    if (s_initialized == 0) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        SSL_library_init();
        SSL_load_error_strings();
#else
        OPENSSL_init_ssl((OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS), NULL);
#endif
        s_initialized = 1;
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_CTX* ctx = SSL_CTX_new(SSLv23_method());
#else
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
#endif
    if (ctx == NULL) return NULL;
    int mode = SSL_VERIFY_NONE;
    const char* ca_file = NULL;
    const char* ca_path = NULL;
    if (param) {
        if (param->ca_file && *param->ca_file) {
            ca_file = param->ca_file;
        }
        if (param->ca_path && *param->ca_path) {
            ca_path = param->ca_path;
        }
        if (ca_file || ca_path) {
            if (!SSL_CTX_load_verify_locations(ctx, ca_file, ca_path)) {
                fprintf(stderr, "ssl ca_file/ca_path failed!\n");
                goto error;
            }
        }

        if (param->crt_file && *param->crt_file) {
            if (!SSL_CTX_use_certificate_file(ctx, param->crt_file, SSL_FILETYPE_PEM)) {
                fprintf(stderr, "ssl crt_file error!\n");
                goto error;
            }
        }

        if (param->key_file && *param->key_file) {
            if (!SSL_CTX_use_PrivateKey_file(ctx, param->key_file, SSL_FILETYPE_PEM)) {
                fprintf(stderr, "ssl key_file error!\n");
                goto error;
            }
            if (!SSL_CTX_check_private_key(ctx)) {
                fprintf(stderr, "ssl key_file check failed!\n");
                goto error;
            }
        }

        if (param->verify_peer) {
            mode = SSL_VERIFY_PEER;
            if (param->endpoint == HSSL_SERVER) {
                mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            }
        }
    }
    if (mode == SSL_VERIFY_PEER && !ca_file && !ca_path) {
        SSL_CTX_set_default_verify_paths(ctx);
    }

#ifdef SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
    // SSL_CTX_set_mode(ctx, SSL_CTX_get_mode(ctx) | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#endif
    SSL_CTX_set_verify(ctx, mode, NULL);
    return ctx;
error:
    SSL_CTX_free(ctx);
    return NULL;
}
tunnel_t *newWolfSSLServer(node_instance_context_t *instance_info)
{

    wssl_server_state_t *state = malloc(sizeof(wssl_server_state_t));
    memset(state, 0, sizeof(wssl_server_state_t));

    ssl_ctx_opt_t *ssl_param = malloc(sizeof(ssl_ctx_opt_t));
    memset(ssl_param, 0, sizeof(ssl_ctx_opt_t));
    const cJSON *settings = instance_info->node_settings_json;

    if (!(cJSON_IsObject(settings) && settings->child != NULL))
    {
        LOGF("JSON Error: WolfSSLServer->settings (object field) : The object was empty or invalid.");
        return NULL;
    }

    if (!getStringFromJsonObject((char **)&(ssl_param->crt_file), settings, "cert-file"))
    {
        LOGF("JSON Error: WolfSSLServer->settings->cert-file (string field) : The data was empty or invalid.");
        return NULL;
    }
    if (strlen(ssl_param->crt_file) == 0)
    {
        LOGF("JSON Error: WolfSSLServer->settings->cert-file (string field) : The data was empty.");
        return NULL;
    }

    if (!getStringFromJsonObject((char **)&(ssl_param->key_file), settings, "key-file"))
    {
        LOGF("JSON Error: WolfSSLServer->settings->key-file (string field) : The data was empty or invalid.");
        return NULL;
    }
    if (strlen(ssl_param->key_file) == 0)
    {
        LOGF("JSON Error: WolfSSLServer->settings->key-file (string field) : The data was empty.");
        return NULL;
    }

    ssl_param->verify_peer = 0; // no mtls
    ssl_param->endpoint = HSSL_SERVER;
    state->ssl_context = ssl_ctx_new(ssl_param);

    // dont do that with APPLE TLS -_-
    free((char *)ssl_param->crt_file);
    free((char *)ssl_param->key_file);
    free(ssl_param);

    if (state->ssl_context == NULL)
    {
        LOGF("Could not create node ssl context");
        return NULL;
    }

    SSL_CTX_set_alpn_select_cb(state->ssl_context, on_alpn_select, NULL);

    tunnel_t *t = newTunnel();
    t->state = state;

    t->upStream = &openSSLUpStream;
    t->packetUpStream = &openSSLPacketUpStream;
    t->downStream = &openSSLDownStream;
    t->packetDownStream = &openSSLPacketDownStream;
    atomic_thread_fence(memory_order_release);
    return t;
}

void apiWolfSSLServer(tunnel_t *self, char *msg)
{
    LOGE("openssl-server API NOT IMPLEMENTED"); // TODO
}

tunnel_t *destroyWolfSSLServer(tunnel_t *self)
{
    LOGE("openssl-server DESTROY NOT IMPLEMENTED"); // TODO
    return NULL;
}
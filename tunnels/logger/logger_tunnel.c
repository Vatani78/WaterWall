#include "logger_tunnel.h"
#include "loggers/network_logger.h"

#define STATE(x) ((logger_tunnel_state_t *)((x)->state))
#define CSTATE(x) ((logger_tunnel_con_state_t *)((((x)->line->chains_state)[self->chain_index])))
#define CSTATE_MUT(x) ((x)->line->chains_state)[self->chain_index]
#define ISALIVE(x) (CSTATE(x) != NULL)

static inline size_t min(size_t x, size_t y) { return (((x) < (y)) ? (x) : (y)); }

typedef struct logger_tunnel_state_s
{

} logger_tunnel_state_t;

typedef struct logger_tunnel_con_state_s
{

} logger_tunnel_con_state_t;

static inline void upStream(tunnel_t *self, context_t *c)
{

    if (c->payload != NULL)
    {

        LOGD("upstream: %zu bytes [ %.*s ]", bufLen(c->payload), min(bufLen(c->payload), 200) ,rawBuf(c->payload));
        if (self->up != NULL)
        {
            self->up->upStream(self->up, c);
        }
        else
        {
            DISCARD_CONTEXT(c);
            destroyContext(c);
        }
    }
    else
    {
        if (c->init)
        {
            LOGD("upstream init");
            if (self->up != NULL)
            {
                self->up->upStream(self->up, c);
            }
            else
            {
                context_t *reply = newContext(c->line);
                reply->est = true;
                destroyContext(c);
                self->dw->downStream(self->dw, reply);
            }
        }
        if (c->fin)
        {
            LOGD("upstream fin");
            if (self->up != NULL)
            {
                self->up->upStream(self->up, c);
            }
        }
    }
}

static inline void downStream(tunnel_t *self, context_t *c)
{

    if (c->payload != NULL)
    {

        LOGD("downstream: %zu bytes [ %.*s ]", bufLen(c->payload), min(bufLen(c->payload), 20), rawBuf(c->payload));
        if (self->dw != NULL)
        {
            self->dw->downStream(self->dw, c);
        }
        else
        {
            DISCARD_CONTEXT(c);
            destroyContext(c);
        }
    }
    else
    {
        if (c->init)
        {
            LOGD("downstream init");
            if (self->dw != NULL)
            {
                self->dw->downStream(self->dw, c);
            }
            else
            {
                context_t *reply = newContext(c->line);
                reply->est = true;
                destroyContext(c);
                self->up->upStream(self->up, reply);
            }
        }
        if (c->fin)
        {
            LOGD("downstream fin");
            if (self->dw != NULL)
            {
                self->dw->downStream(self->dw, c);
            }
        }
        if (c->est)
        {
            LOGD("downstream est");
            if (self->dw != NULL)
            {
                self->dw->downStream(self->dw, c);
            }
        }
    }
}

static void loggerTunnelUpStream(tunnel_t *self, context_t *c)
{
    upStream(self, c);
}
static void loggerTunnelPacketUpStream(tunnel_t *self, context_t *c)
{
    upStream(self, c);
}
static void loggerTunnelDownStream(tunnel_t *self, context_t *c)
{
    downStream(self, c);
}
static void loggerTunnelPacketDownStream(tunnel_t *self, context_t *c)
{
    downStream(self, c);
}

tunnel_t *newLoggerTunnel(node_instance_context_t *instance_info)
{

    tunnel_t *t = newTunnel();

    t->upStream = &loggerTunnelUpStream;
    t->packetUpStream = &loggerTunnelPacketUpStream;
    t->downStream = &loggerTunnelDownStream;
    t->packetDownStream = &loggerTunnelPacketDownStream;
    return t;
}

void apiLoggerTunnel(tunnel_t *self, char *msg)
{
    LOGE("logger-tunnel API NOT IMPLEMENTED"); // TODO
}

tunnel_t *destroyLoggerTunnel(tunnel_t *self)
{
    LOGE("logger-tunnel DESTROY NOT IMPLEMENTED"); // TODO
    return NULL;
}
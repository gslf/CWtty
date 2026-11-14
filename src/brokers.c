/*
 * brokers.c - The MQTT brokers of the rooms.
 *
 * The primary is an EMQX Serverless deployment (TLS only, with credentials
 * that every copy of the app carries: its access control only allows the
 * cwtty/ topics). The backup is HiveMQ's public broker. The credentials
 * are not secret, and the packets of a private room are encrypted end to
 * end: a broker never sees their content.
 */
#include "brokers.h"

const broker_info brokers[BROKERS] = {
    /* EMQX Serverless: filled in from the deployment's overview page. */
    {"EMQX", "", 8883, 1, "", "", "", 0},
    {"HiveMQ", "broker.hivemq.com", 1883, 0, "wss://broker.hivemq.com:8884/mqtt", "", "", 0},
};

int broker_configured(int i)
{
    return i >= 0 && i < BROKERS && brokers[i].host && brokers[i].host[0];
}

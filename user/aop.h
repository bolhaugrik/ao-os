/* AOP-kliens ring 3-ban: kapcsolat a hiddal, kezfogas, PSK-titkositas, keretek (docs/AOP.md).
 * Az agentd es a projector kozos resze. Egyszerre egy kapcsolat. */
#pragma once
#include "aolib.h"

enum {
    AOP_HELLO = 1, AOP_HELLO_OK, AOP_CONTEXT, AOP_PROMPT, AOP_DELTA, AOP_TOOL_CALL, AOP_TOOL_RESULT,
    AOP_END, AOP_ERR, AOP_PING, AOP_PONG, AOP_FILE, AOP_CLIP_GET, AOP_CLIP, AOP_PROJECT, AOP_IMPRINT,
    AOP_FETCH,
};

#define AOP_PAYLOAD_MAX 65536
extern u8 aop_payload[AOP_PAYLOAD_MAX + 64];     /* a fogadott keret tartalma, NUL-lal zarva */

/* Hid cime (/state/ai/bridge, /etc/ai/bridge), PSK (/state/ai/psk), kapcsolodas, kezfogas.
 * quiet: nincs "[agent ...]" es "[hid: ...]" sor. Visszaad 0-t, vagy a program kilepesi kodjat:
 * 2 nincs cim, 3 kapcsolodas, 4 kezfogas, 5 a hid ERR-t kuldott (kiirva), 6 PSK-eltelres. */
int  aop_connect(const char *agent_name, bool quiet);
int  aop_send(u16 type, const void *data, usize len);
int  aop_recv(u16 *type, usize *len);            /* 0 vagy hibakod; PING-re maga valaszol */
void aop_close(void);
bool aop_encrypted(void);
const char *aop_model(void);                     /* a HELLO_OK "model=..." erteke */

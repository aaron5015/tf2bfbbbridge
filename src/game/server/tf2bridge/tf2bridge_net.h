#ifndef TF2BRIDGE_NET_H
#define TF2BRIDGE_NET_H

// Plain UDP side of the BFBB <-> TF2 bridge. Deliberately includes NO Source
// headers (cbase.h and winsock2.h do not get along), so the socket code lives
// in its own translation unit behind this tiny interface.

#include "iTF2BridgeProto.h"

bool TF2Bridge_NetOpen(); // binds 127.0.0.1:BRIDGE_PORT_TF2, non-blocking
void TF2Bridge_NetClose();
bool TF2Bridge_NetIsOpen();

// Drains the socket. Returns true and fills `out` if at least one NEW state
// packet arrived; `out` is the newest one.
bool TF2Bridge_NetRecvState(BridgeStatePacket* out);

// Sends one intent packet to BFBB (127.0.0.1:BRIDGE_PORT_BFBB). seq is filled in.
void TF2Bridge_NotifyWeaponFired();
void TF2Bridge_NetSendIntent(BridgeIntentPacket* pkt);

#endif

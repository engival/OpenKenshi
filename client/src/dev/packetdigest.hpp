#pragma once
// The packet digest and the packet poison (kenshi-k16.2.205.12.93; overlap review kenshi-k16.2.205.12.85 C1 and B3). Dev builds only
// (KCLIENT_DEV_TOOLS, src/dev/): the game calls them through the two hooks below, inline no-ops with the option OFF.
//
// digest: with KCLIENT_PACKET_DIGEST set, the end of logic_frame prints one stderr line per packet, `PKTDIGEST serial=N path=P block=hash ...`,
// one 64-bit hash per FramePacket block. Off, lockstep and (later) overlap run the same script and must print equal lines: a differing block
// names the first field group the modes disagree on (an unequal pixel hunt becomes a diff). Timing (logicMs) and the render-owned handles are
// not hashed; GPU buffer ids in the character plan are (resolved on the render side but equal across modes by construction).
//
// poison: with KCLIENT_PACKET_POISON set, the top of logic_frame wipes the slot it is about to fill: every container cleared, every float and
// double NaN, every count -1, every flag true. A field a frame does not write (RENDER_THREAD.md 7.1 rule 2, "every field written every frame")
// then reaches the draw as poison and shows in scene_test as a change. Blocks that are legitimately written only under a condition must be
// guarded by the same condition in the draw; the poison proves it.
#include <cstdint>

struct FramePacket;

namespace dev
{
#ifdef KCLIENT_DEV_TOOLS
	void packetPoison(FramePacket& pk);	// no-op unless KCLIENT_PACKET_POISON is set
	void packetDigest(const FramePacket& pk);	// no-op unless KCLIENT_PACKET_DIGEST is set
#else
	inline void packetPoison(FramePacket&) {}
	inline void packetDigest(const FramePacket&) {}
#endif
}

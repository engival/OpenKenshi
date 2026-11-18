#!/usr/bin/env python3
"""Wwise soundbank (.bnk) parser focused on the interactive-music objects.

Format knowledge from bnnm/wwiser (https://github.com/bnnm/wwiser), read as
documentation only; this module is an independent implementation, scoped to
bank version 112 (Wwise 2014.x).  Per-object layouts follow the version-112
branches documented in wwiser's wparser.py; the function that documents each
object is cited in a comment next to its decoder below.  Marker ids for the
Entry/Exit cues come from wwiser's generator/render/bnode_markers.py.

Usage:
	wwise_bnk.py <bank.bnk> [more.bnk ...] [--json | --tree | --ids]

	(default)  readable dump of every chunk and every decoded HIRC object
	--json     full machine-readable dump (one JSON document per bank)
	--tree     music hierarchy only (switch -> playlists -> segments -> tracks)
	--ids      TSV of state/switch/event/RTPC/trigger ids and who references them

Scope:
	BKHD, DIDX, STID, STMG (global settings) and HIRC are parsed.  The DATA
	payload is never read; only its position is noted.  HIRC objects of types
	Music Segment/Track/Switch/Random-Sequence, Event, Action and State are
	decoded in full; all other types are recorded as (type, id, size) only.
	Every decoded object must consume exactly its declared size; any
	difference (or exception) is recorded on the object and summarised in
	bank["mismatches"].
"""
import json
import struct
import sys

# ---------------------------------------------------------------------------
# enums (value names from wwiser wdefs.py, version-112 tables)

HIRC_TYPES = {
	0x01: "State", 0x02: "Sound", 0x03: "Action", 0x04: "Event",
	0x05: "Random/Sequence Container", 0x06: "Switch Container",
	0x07: "Actor-Mixer", 0x08: "Bus", 0x09: "Layer Container",
	0x0A: "Music Segment", 0x0B: "Music Track", 0x0C: "Music Switch",
	0x0D: "Music Random/Sequence", 0x0E: "Attenuation", 0x0F: "DialogueEvent",
	0x10: "FeedbackBus", 0x11: "FeedbackNode", 0x12: "FxShareSet",
	0x13: "FxCustom", 0x14: "Auxiliary Bus", 0x15: "LFO", 0x16: "Envelope",
	0x17: "AudioDevice",
}
T_STATE, T_SOUND, T_ACTION, T_EVENT = 1, 2, 3, 4
T_SEGMENT, T_TRACK, T_MSWITCH, T_MRANSEQ = 0x0A, 0x0B, 0x0C, 0x0D

CURVES = ["Log3", "Sine", "Log1", "InvSCurve", "Linear", "SCurve", "Exp1",
	"SineRecip", "Exp3", "Constant"]
SYNC_TYPES = ["Immediate", "NextGrid", "NextBar", "NextBeat", "NextMarker",
	"NextUserMarker", "EntryMarker", "ExitMarker", "ExitNever",
	"LastExitPosition"]
GROUP_TYPES = ["Switch", "State"]
TREE_MODES = ["BestMatch", "Weighted"]
RS_TYPES = ["ContinuousSequence", "StepSequence", "ContinuousRandom",
	"StepRandom"]
ENTRY_TYPES = ["EntryMarker", "SameTime", "RandomMarker", "RandomUserMarker",
	"LastExitTime"]
TRACK_TYPES = ["Normal", "Random", "Sequence", "Switch"]
CLIP_AUTO_TYPES = ["Volume", "LPF", "HPF", "FadeIn", "FadeOut"]
VALUE_MEANING = ["Default", "Independent", "Offset"]
SOURCE_STREAM = ["Data/bnk", "PrefetchStreaming", "Streaming"]
RTPC_TYPES = {0: "GameParameter", 1: "MIDI", 2: "Modulator"}

# AkPropID, wwiser AkPropID_113 (used for versions 90..113)
PROP_IDS = {
	0x00: "Volume", 0x01: "LFE", 0x02: "Pitch", 0x03: "LPF", 0x04: "HPF",
	0x05: "BusVolume", 0x06: "Priority", 0x07: "PriorityDistanceOffset",
	0x08: "FeedbackVolume", 0x09: "FeedbackLPF", 0x0A: "MuteRatio",
	0x0B: "PAN_LR", 0x0C: "PAN_FR", 0x0D: "CenterPCT", 0x0E: "DelayTime",
	0x0F: "TransitionTime", 0x10: "Probability", 0x11: "DialogueMode",
	0x12: "UserAuxSendVolume0", 0x13: "UserAuxSendVolume1",
	0x14: "UserAuxSendVolume2", 0x15: "UserAuxSendVolume3",
	0x16: "GameAuxSendVolume", 0x17: "OutputBusVolume", 0x18: "OutputBusHPF",
	0x19: "OutputBusLPF", 0x1A: "HDRBusThreshold", 0x1B: "HDRBusRatio",
	0x1C: "HDRBusReleaseTime", 0x1D: "HDRBusGameParam",
	0x1E: "HDRBusGameParamMin", 0x1F: "HDRBusGameParamMax",
	0x20: "HDRActiveRange", 0x21: "MakeUpGain", 0x22: "LoopStart",
	0x23: "LoopEnd", 0x24: "TrimInTime", 0x25: "TrimOutTime",
	0x26: "FadeInTime", 0x27: "FadeOutTime", 0x28: "FadeInCurve",
	0x29: "FadeOutCurve", 0x2A: "LoopCrossfadeDuration",
	0x2B: "CrossfadeUpCurve", 0x2C: "CrossfadeDownCurve",
	0x2D: "MidiTrackingRootNote", 0x2E: "MidiPlayOnNoteType",
	0x2F: "MidiTransposition", 0x30: "MidiVelocityOffset",
	0x31: "MidiKeyRangeMin", 0x32: "MidiKeyRangeMax",
	0x33: "MidiVelocityRangeMin", 0x34: "MidiVelocityRangeMax",
	0x35: "MidiChannelMask", 0x36: "PlaybackSpeed", 0x37: "MidiTempoSource",
	0x38: "MidiTargetNode", 0x39: "AttachedPluginFXID", 0x3A: "Loop",
	0x3B: "InitialDelay",
}
PROP_ID_VALUES = {"AttachedPluginFXID", "MidiTargetNode"}

# AkRTPC_ParameterID, wwiser AkRTPC_ParameterID_113 (names only for the
# parameters a music node can realistically target)
RTPC_PARAMS = {
	0x0: "Volume", 0x1: "LFE", 0x2: "Pitch", 0x3: "LPF", 0x4: "HPF",
	0x5: "BusVolume", 0x6: "InitialDelay",
	0x7: "PlayMechanismSpecialTransitionsValue", 0x8: "Priority",
	0x9: "MaxNumInstances", 0xA: "PositioningType",
	0xB: "Positioning_Divergence_Center_PCT",
	0x13: "GameAuxSendVolume", 0x14: "Position_PAN_X_2D",
	0x15: "Position_PAN_Y_2D", 0x24: "MakeUpGain",
	0x25: "Position_PAN_X_3D", 0x26: "Position_PAN_Y_3D",
	0x29: "PlaybackSpeed", 0x38: "UserAuxSendVolume0",
	0x39: "UserAuxSendVolume1", 0x3A: "UserAuxSendVolume2",
	0x3B: "UserAuxSendVolume3", 0x3C: "OutputBusVolume",
	0x3D: "OutputBusHPF", 0x3E: "OutputBusLPF", 0x3F: "MuteRatio",
}

# action type high byte -> class (wwiser wparser_cls.CAkAction__Create, v>56)
ACTION_CLASSES = {
	0x01: "Stop", 0x02: "Pause", 0x03: "Resume", 0x04: "Play",
	0x05: "PlayAndContinue", 0x06: "Mute", 0x07: "Mute",
	0x08: "SetAkProp", 0x09: "SetAkProp", 0x0A: "SetAkProp",
	0x0B: "SetAkProp", 0x0C: "SetAkProp", 0x0D: "SetAkProp",
	0x0E: "SetAkProp", 0x0F: "SetAkProp", 0x10: "UseState", 0x11: "UseState",
	0x12: "SetState", 0x13: "SetGameParameter", 0x14: "SetGameParameter",
	0x15: "Event", 0x16: "Event", 0x17: "Event", 0x19: "SetSwitch",
	0x1A: "BypassFX", 0x1B: "BypassFX", 0x1C: "Break", 0x1D: "Trigger",
	0x1E: "Seek", 0x1F: "Release", 0x20: "SetAkProp", 0x21: "PlayEvent",
	0x22: "ResetPlaylist", 0x23: "PlayEventUnknown", 0x30: "SetAkProp",
}
# scope byte (low byte of the action type) -> readable scope (wwiser AkActionType_062)
ACTION_SCOPES = {0x01: "", 0x02: "(target)", 0x03: "(target, override)",
	0x04: "(all)", 0x05: "(all, override)",
	0x08: "(all except)", 0x09: "(all except, override)",
	0x11: "", 0x20: "", 0x00: ""}
ACTION_NAMES = {
	0x0403: "Play", 0x0503: "PlayAndContinue",
	0x0102: "Stop_E", 0x0103: "Stop_E_O", 0x0104: "Stop_ALL",
	0x0105: "Stop_ALL_O", 0x0108: "Stop_AE", 0x0109: "Stop_AE_O",
	0x0202: "Pause_E", 0x0203: "Pause_E_O", 0x0204: "Pause_ALL",
	0x0205: "Pause_ALL_O", 0x0208: "Pause_AE", 0x0209: "Pause_AE_O",
	0x0302: "Resume_E", 0x0303: "Resume_E_O", 0x0304: "Resume_ALL",
	0x0305: "Resume_ALL_O", 0x0308: "Resume_AE", 0x0309: "Resume_AE_O",
	0x1204: "SetState", 0x1901: "SetSwitch",
	0x1302: "SetGameParameter", 0x1303: "SetGameParameter_O",
	0x1402: "ResetGameParameter", 0x1403: "ResetGameParameter_O",
	0x1511: "StopEvent", 0x1611: "PauseEvent", 0x1711: "ResumeEvent",
	0x0A02: "SetVolume_M", 0x0A03: "SetVolume_O", 0x0B02: "ResetVolume_M",
	0x0B03: "ResetVolume_O", 0x0B04: "ResetVolume_ALL",
	0x0602: "Mute_M", 0x0603: "Mute_O", 0x0702: "Unmute_M",
	0x0703: "Unmute_O", 0x0704: "Unmute_ALL",
	0x0802: "SetPitch_M", 0x0803: "SetPitch_O", 0x0902: "ResetPitch_M",
	0x0E02: "SetLPF_M", 0x0E03: "SetLPF_O", 0x0F02: "ResetLPF_M",
	0x2002: "SetHPF_M", 0x2003: "SetHPF_O", 0x3002: "ResetHPF_M",
	0x0C02: "SetBusVolume_M", 0x0C03: "SetBusVolume_O",
	0x0D02: "ResetBusVolume_M", 0x0D03: "ResetBusVolume_O",
	0x1D00: "Trigger", 0x1D01: "Trigger_O", 0x1D02: "Trigger_E",
	0x1C02: "Break_E", 0x1C03: "Break_E_O",
	0x1E02: "Seek_E", 0x1E03: "Seek_E_O", 0x1E04: "Seek_ALL",
	0x2103: "PlayEvent", 0x2202: "ResetPlaylist_E", 0x2203: "ResetPlaylist_E_O",
	0x1F02: "Release", 0x1F03: "Release_O",
	0x1A02: "BypassFX_M", 0x1A03: "BypassFX_O", 0x1B02: "ResetBypassFX_M",
	0x1B03: "ResetBypassFX_O", 0x1B04: "ResetBypassFX_ALL",
	0x1002: "UseState_E", 0x1102: "UnuseState_E",
}

ENTRY_CUE_ID = 43573010      # wwiser generator/render/bnode_markers.py
EXIT_CUE_ID = 1539036744
ANY_NODE = 0xFFFFFFFF


def enum(table, value):
	if 0 <= value < len(table):
		return table[value]
	return "?%d" % value


class ParseError(Exception):
	pass


class Reader:
	"""Bounds-checked little-endian cursor over a bytes object."""

	def __init__(self, data, pos=0, end=None):
		self.d = data
		self.pos = pos
		self.end = len(data) if end is None else end

	def _take(self, fmt, size):
		if self.pos + size > self.end:
			raise ParseError("read past end of object (need %d bytes at +%d, "
							 "object ends at +%d)" % (size, self.pos, self.end))
		v = struct.unpack_from(fmt, self.d, self.pos)[0]
		self.pos += size
		return v

	def u8(self):
		return self._take("<B", 1)

	def u16(self):
		return self._take("<H", 2)

	def s16(self):
		return self._take("<h", 2)

	def u32(self):
		return self._take("<I", 4)

	def s32(self):
		return self._take("<i", 4)

	def f32(self):
		return self._take("<f", 4)

	def f64(self):
		return self._take("<d", 8)

	def bytes(self, n):
		if self.pos + n > self.end:
			raise ParseError("read past end of object (need %d bytes)" % n)
		b = self.d[self.pos:self.pos + n]
		self.pos += n
		return b

	def left(self):
		return self.end - self.pos


def uni(raw):
	"""AkPropValue union: show as float unless the bit pattern is a small int."""
	if raw == 0:
		return 0
	if raw < 0x00800000 or raw >= 0xFFFF0000:
		return raw if raw < 0x80000000 else raw - (1 << 32)
	return round(struct.unpack("<f", struct.pack("<I", raw))[0], 6)


# ---------------------------------------------------------------------------
# common sub-structures

def read_prop_bundle(r, ranged=False):
	"""AkPropBundle<AkPropValue> / <RANGED_MODIFIERS<AkPropValue>>.
	wwiser: AkPropBundle_AkPropValue_unsigned_char___SetInitialParams and
	AkPropBundle_RANGED_MODIFIERS_AkPropValue__unsigned_char___SetInitialParams."""
	n = r.u8()
	ids = [r.u8() for _ in range(n)]
	out = {}
	for pid in ids:
		name = PROP_IDS.get(pid, "prop_0x%02X" % pid)
		if ranged:
			out[name] = [uni(r.u32()), uni(r.u32())]
		else:
			raw = r.u32()
			out[name] = raw if name in PROP_ID_VALUES else uni(raw)
	return out


def read_rtpc_graph(r, count):
	return [{"from": round(r.f32(), 6), "to": round(r.f32(), 6),
			 "interp": enum(CURVES, r.u32())} for _ in range(count)]


def read_music_fade(r):
	return {"time_ms": r.s32(), "curve": enum(CURVES, r.u32()),
			"offset_ms": r.s32()}


def read_fx_params(r):
	"""wwiser: CAkParameterNode__SetInitialFxParams (v<=145 branch)."""
	d = {"override_parent": r.u8()}
	n = r.u8()
	fx = []
	if n:
		d["bypass_bits"] = r.u8()
		for _ in range(n):
			fx.append({"index": r.u8(), "fx_id": r.u32(),
					   "is_shareset": r.u8(), "is_rendered": r.u8()})
	d["fx"] = fx
	return d


def read_positioning(r):
	"""wwiser: CAkParameterNodeBase__SetPositioningParams (89<v<=122)."""
	bits = r.u8()
	d = {"bits": bits}
	if not bits & 1:
		return d
	if not (bits >> 3) & 1:
		return d
	bits3d = r.u8()
	d["bits3d"] = bits3d
	d["attenuation_id"] = r.u32()
	if (bits3d & 3) != 1:
		d["path_mode"] = r.u8()
		d["transition_time"] = r.s32()
		nv = r.u32()
		d["vertices"] = [[r.f32(), r.f32(), r.f32(), r.s32()]
						 for _ in range(nv)]
		ni = r.u32()
		d["path_items"] = [[r.u32(), r.u32()] for _ in range(ni)]
		d["automation"] = [[r.f32(), r.f32(), r.f32()] for _ in range(ni)]
	return d


def read_node_base(r, ctx):
	"""wwiser: CAkParameterNodeBase__SetNodeBaseParams (v112 path, with
	SetInitialFxParams, SetInitialParams, SetPositioningParams, SetAuxParams,
	SetAdvSettingsParams, ReadStateChunk, SetInitialRTPC, ReadFeedbackInfo)."""
	d = {"fx": read_fx_params(r)}
	d["override_attachment"] = r.u8()
	d["override_bus"] = r.u32()
	d["parent"] = r.u32()
	d["priority_bits"] = r.u8()
	d["props"] = read_prop_bundle(r)
	d["prop_ranges"] = read_prop_bundle(r, ranged=True)
	d["positioning"] = read_positioning(r)

	# CAkParameterNodeBase__SetAuxParams
	aux = r.u8()
	d["aux_bits"] = aux
	d["aux_ids"] = [r.u32() for _ in range(4)] if (aux >> 3) & 1 else []

	# CAkParameterNode__SetAdvSettingsParams (v>89)
	d["adv"] = {"bits1": r.u8(), "virtual_queue": r.u8(),
				"max_instances": r.u16(), "below_threshold": r.u8(),
				"bits2": r.u8()}

	# CAkParameterNodeBase__ReadStateChunk (53<=v<=122)
	groups = []
	for _ in range(r.u32()):
		g = {"group": r.u32(), "sync": enum(SYNC_TYPES, r.u8())}
		n = r.u16()
		g["states"] = [{"state": r.u32(), "instance": r.u32()}
					   for _ in range(n)]
		groups.append(g)
	d["state_chunk"] = groups

	# SetInitialRTPC_CAkParameterNodeBase_ (v<=141: u16 count)
	rtpcs = []
	for _ in range(r.u16()):
		e = {"rtpc_id": r.u32(), "rtpc_type": RTPC_TYPES.get(r.u8(), "?"),
			 "accum": r.u8()}
		pid = r.u8()
		e["param"] = RTPC_PARAMS.get(pid, "param_0x%02X" % pid)
		e["curve_id"] = r.s32()
		e["scaling"] = r.u8()
		e["points"] = read_rtpc_graph(r, r.u16())
		rtpcs.append(e)
	d["rtpc"] = rtpcs

	# CAkParameterNodeBase__ReadFeedbackInfo (only when the bank flags it)
	if ctx["feedback"]:
		d["feedback_bus"] = r.u32()
	return d


def read_music_node(r, ctx):
		"""wwiser: CAkMusicNode__SetMusicNodeParams."""
		d = {"flags": r.u8()}
		d["base"] = read_node_base(r, ctx)
		d["children"] = [r.u32() for _ in range(r.u32())]
		d["meter"] = {"grid_period_ms": r.f64(), "grid_offset_ms": r.f64(),
									"tempo_bpm": round(r.f32(), 4), "beats_per_bar": r.u8(),
									"beat_value": r.u8()}
		d["meter_override"] = r.u8()
		stingers = []
		for _ in range(r.u32()):
				stingers.append({"trigger_id": r.u32(), "segment_id": r.u32(),
												 "sync": enum(SYNC_TYPES, r.u32()),
												 "cue_filter_hash": r.u32(),
												 "dont_repeat_ms": r.s32(),
												 "segments_look_ahead": r.u32()})
		d["stingers"] = stingers
		return d


def read_media_source(r):
	"""wwiser: CAkBankMgr__LoadSource (v90..112 branch)."""
	plugin = r.u32()
	stream = r.u8()
	s = {"plugin": "0x%08X" % plugin, "stream": enum(SOURCE_STREAM, stream),
		 "source_id": r.u32(), "file_id": r.u32()}
	if stream != 2:
		s["file_offset"] = r.u32()
	s["in_memory_size"] = r.u32()
	s["source_bits"] = r.u8()
	if plugin & 0xF in (2, 5):
		size = r.u32()
		s["plugin_param_bytes"] = size
		r.bytes(size)
	return s


# ---------------------------------------------------------------------------
# HIRC object decoders; each takes a Reader positioned after the object id

def dec_state(r, ctx):
	"""wwiser: CAkBankMgr__ReadState / CAkState__SetInitialValues (v<=126)."""
	n = r.u8()
	ids = [r.u8() for _ in range(n)]
	props = {}
	for pid in ids:
		props[RTPC_PARAMS.get(pid, "param_0x%02X" % pid)] = round(r.f32(), 6)
	return {"props": props}


def dec_event(r, ctx):
	"""wwiser: CAkBankMgr__ReadEvent / CAkEvent__SetInitialValues (v<=122)."""
	return {"actions": [r.u32() for _ in range(r.u32())]}


def read_except_list(r):
	"""wwiser: CAkActionExcept__SetExceptParams (v<=122)."""
	return [{"id": r.u32(), "is_bus": r.u8()} for _ in range(r.u32())]


def dec_action(r, ctx):
	"""wwiser: CAkBankMgr__ReadAction, CAkAction__SetInitialValues,
	CAkAction__Create (class dispatch) and the CAkAction*__SetActionParams
	functions (v112 branches)."""
	atype = r.u16()
	cls = ACTION_CLASSES.get(atype >> 8)
	d = {"action_type": "0x%04X" % atype,
		 "action_name": ACTION_NAMES.get(atype, "?"), "class": cls}
	d["target_id"] = r.u32()          # idExt
	d["target_is_bus"] = r.u8() & 1
	d["props"] = read_prop_bundle(r)
	d["prop_ranges"] = read_prop_bundle(r, ranged=True)
	if cls is None:
		raise ParseError("unknown action type 0x%04X" % atype)

	if cls in ("Stop", "Pause", "Resume", "SetAkProp", "SetGameParameter",
			   "Mute", "ResetPlaylist"):
		# CAkActionActive / CAkActionSetValue: fade curve, specific, except
		d["fade_curve"] = enum(CURVES, r.u8() & 0x1F)
		if cls in ("Pause", "Resume"):
			d["flags"] = r.u8()
		elif cls == "SetAkProp":
			d["value_meaning"] = enum(VALUE_MEANING, r.u8())
			d["value"] = {"base": r.f32(), "min": r.f32(), "max": r.f32()}
		elif cls == "SetGameParameter":
			d["bypass_transition"] = r.u8()
			d["value_meaning"] = enum(VALUE_MEANING, r.u8())
			d["value"] = {"base": r.f32(), "min": r.f32(), "max": r.f32()}
		d["except"] = read_except_list(r)
	elif cls in ("Play", "PlayAndContinue", "PlayEventUnknown"):
		d["fade_curve"] = enum(CURVES, r.u8() & 0x1F)
		d["bank_id"] = r.u32()
	elif cls == "SetState":
		d["state_group"] = r.u32()
		d["state"] = r.u32()
	elif cls == "SetSwitch":
		d["switch_group"] = r.u32()
		d["switch"] = r.u32()
	elif cls == "BypassFX":
		d["is_bypass"] = r.u8()
		d["target_mask"] = r.u8()
		d["except"] = read_except_list(r)
	elif cls == "Seek":
		d["relative_to_duration"] = r.u8()
		d["seek"] = {"value": r.f32(), "min": r.f32(), "max": r.f32()}
		d["snap_to_marker"] = r.u8()
		d["except"] = read_except_list(r)
	# UseState, Event*, Break, Trigger, Release, PlayEvent: no extra fields
	return d


def dec_segment(r, ctx):
	"""wwiser: CAkBankMgr__StdBankRead_CAkMusicSegment_..., CAkMusicSegment__SetInitialValues."""
	d = read_music_node(r, ctx)
	d["duration_ms"] = r.f64()
	markers = []
	for _ in range(r.u32()):
		m = {"id": r.u32(), "pos_ms": r.f64()}
		n = r.u32()
		m["name"] = r.bytes(n).rstrip(b"\0").decode("utf-8", "replace") if n else ""
		if m["id"] == ENTRY_CUE_ID:
			m["role"] = "entry"
		elif m["id"] == EXIT_CUE_ID:
			m["role"] = "exit"
		markers.append(m)
	d["markers"] = markers
	return d


def dec_track(r, ctx):
		"""wwiser: CAkBankMgr__ReadSourceParent_CAkMusicTrack_, CAkMusicTrack__SetInitialValues,
		TrackSwitchInfo__SetSwitchParams / __SetTransParams (v112)."""
		d = {"overrides": r.u8()}
		d["sources"] = [read_media_source(r) for _ in range(r.u32())]
		clips = []
		n = r.u32()
		for _ in range(n):
				clips.append({"track_index": r.u32(), "source_id": r.u32(),
											"play_at_ms": r.f64(), "begin_trim_ms": r.f64(),
											"end_trim_ms": r.f64(), "src_duration_ms": r.f64()})
		d["clips"] = clips
		if n:
				d["num_subtracks"] = r.u32()
		autos = []
		for _ in range(r.u32()):
				a = {"clip_index": r.u32(), "type": enum(CLIP_AUTO_TYPES, r.u32())}
				a["points"] = read_rtpc_graph(r, r.u32())
				autos.append(a)
		d["clip_automation"] = autos
		d["base"] = read_node_base(r, ctx)
		ttype = r.u8()
		d["track_type"] = enum(TRACK_TYPES, ttype)
		if ttype == 3:
				d["switch"] = {"group_type": enum(GROUP_TYPES, r.u8()),
											 "group": r.u32(), "default": r.u32(),
											 "assoc": [r.u32() for _ in range(r.u32())]}
				d["trans"] = {"src_fade": read_music_fade(r),
											"sync": enum(SYNC_TYPES, r.u32()),
											"cue_filter_hash": r.u32(),
											"dst_fade": read_music_fade(r)}
		d["look_ahead_ms"] = r.s32()
		return d


def read_trans_rules(r):
	"""wwiser: CAkMusicTransAware__SetMusicTransNodeParams (v>72, v<=132)."""
	rules = []
	for _ in range(r.u32()):
		e = {"src": [r.u32() for _ in range(r.u32())]}
		e["dst"] = [r.u32() for _ in range(r.u32())]
		e["src_rule"] = {"fade": read_music_fade(r),
						 "sync": enum(SYNC_TYPES, r.u32()),
						 "cue_filter_hash": r.u32(),
						 "play_post_exit": r.u8()}
		fade = read_music_fade(r)
		e["dst_rule"] = {"fade": fade, "cue_filter_hash": r.u32(),
						 "jump_to": r.u32(),
						 "entry": enum(ENTRY_TYPES, r.u16()),
						 "play_pre_entry": r.u8(),
						 "match_source_cue_name": r.u8()}
		if r.u8():
			e["transition_object"] = {
				"segment_id": r.u32(), "fade_in": read_music_fade(r),
				"fade_out": read_music_fade(r), "play_pre_entry": r.u8(),
				"play_post_exit": r.u8()}
		rules.append(e)
	return rules


def read_trans_node(r, ctx):
	d = read_music_node(r, ctx)
	d["rules"] = read_trans_rules(r)
	return d


def read_playlist(r):
	"""wwiser: CAkMusicRanSeqCntr__SetInitialValues + parse_playlist_node.
	Items are stored depth-first: item, then its NumChildren sub-items."""
	count = [0]

	def item():
		count[0] += 1
		n = {"segment_id": r.u32(), "item_id": r.u32()}
		nchild = r.u32()
		rs = r.u32()
		n["rs_type"] = "None(leaf)" if rs == 0xFFFFFFFF else enum(RS_TYPES, rs)
		n["loop"] = r.s16()
		n["loop_min"] = r.s16()
		n["loop_max"] = r.s16()
		n["weight"] = r.u32()
		n["avoid_repeat"] = r.u16()
		n["use_weight"] = r.u8()
		n["shuffle"] = r.u8()
		n["children"] = [item() for _ in range(nchild)]
		return n

	root = item()
	return root, count[0]


def dec_mranseq(r, ctx):
	d = read_trans_node(r, ctx)
	d["declared_items"] = r.u32()
	d["playlist"], d["parsed_items"] = read_playlist(r)
	if d["declared_items"] != d["parsed_items"]:
		raise ParseError("playlist item count %d != declared %d"
						 % (d["parsed_items"], d["declared_items"]))
	return d


def read_decision_tree(data, depth, mode_weighted):
	"""wwiser: AkDecisionTree__SetTree / parse_tree_node.  The blob is a flat
	array of 12-byte nodes {key u32; (audio_id u32 | idx u16, count u16);
	weight u16; probability u16}; node 0 is the root, children of a node are
	array[idx : idx+count], nodes at depth == tree depth hold audio node ids."""
	n = len(data) // 12
	if len(data) % 12:
		raise ParseError("tree blob size %d not a multiple of 12" % len(data))
	nodes = [struct.unpack_from("<IIHH", data, i * 12) for i in range(n)]
	seen = set()
	anomalies = []

	def build(i, level):
		if i in seen:
			raise ParseError("decision tree node %d visited twice" % i)
		if i >= n:
			raise ParseError("decision tree node index %d out of range" % i)
		seen.add(i)
		key, a, w, p = nodes[i]
		node = {"key": key, "weight": w, "prob": p}
		if level == depth:
			node["audio_id"] = a
			return node
		idx, cnt = a & 0xFFFF, a >> 16
		node["children"] = [build(idx + k, level + 1) for k in range(cnt)]
		return node

	if n == 0:
		return None, anomalies
	root = build(0, 0)
	if len(seen) != n:
		anomalies.append("decision tree: %d of %d nodes unreachable"
						 % (n - len(seen), n))
	return root, anomalies


def dec_mswitch(r, ctx):
		"""wwiser: CAkBankMgr__StdBankRead_CAkMusicSwitchCntr_..., CAkMusicSwitchCntr__SetInitialValues
		(v>72), CAkMusicSwitchCntr__SetArguments."""
		d = read_trans_node(r, ctx)
		d["continue_playback"] = r.u8()
		depth = r.u32()
		d["tree_depth"] = depth
		groups = [r.u32() for _ in range(depth)]
		gtypes = [r.u8() for _ in range(depth)]
		d["arguments"] = [{"group": g, "group_type": enum(GROUP_TYPES, t)}
											for g, t in zip(groups, gtypes)]
		size = r.u32()
		d["tree_bytes"] = size
		d["mode"] = enum(TREE_MODES, r.u8())
		blob = r.bytes(size)
		d["tree"], anomalies = read_decision_tree(blob, depth, d["mode"] == "Weighted")
		if anomalies:
				d["anomalies"] = anomalies
		return d


DECODERS = {
	T_STATE: dec_state, T_ACTION: dec_action, T_EVENT: dec_event,
	T_SEGMENT: dec_segment, T_TRACK: dec_track, T_MSWITCH: dec_mswitch,
	T_MRANSEQ: dec_mranseq,
}


# ---------------------------------------------------------------------------
# chunks

def parse_bkhd(data, off, size):
	r = Reader(data, off, off + size)
	d = {"version": r.u32(), "bank_id": r.u32(), "language_id": r.u32(),
		 "feedback_flag": r.u32(), "project_id": r.u32()}
	d["padding"] = r.left()
	return d


def parse_didx(data, off, size):
	r = Reader(data, off, off + size)
	out = []
	for _ in range(size // 12):
		out.append({"id": r.u32(), "offset": r.u32(), "size": r.u32()})
	return out


def parse_stid(data, off, size):
	"""wwiser: CAkBankMgr__ProcessStringMappingChunk."""
	r = Reader(data, off, off + size)
	d = {"type": r.u32()}
	entries = []
	for _ in range(r.u32()):
		bid = r.u32()
		n = r.u8()
		entries.append({"bank_id": bid,
						"name": r.bytes(n).decode("utf-8", "replace")})
	d["entries"] = entries
	d["unconsumed"] = r.left()
	return d


def parse_stmg(data, off, size):
	"""wwiser: CAkBankMgr__ProcessGlobalSettingsChunk (v<=118 shape)."""
	r = Reader(data, off, off + size)
	d = {"volume_threshold": r.f32(), "max_voices": r.u16()}
	groups = []
	for _ in range(r.u32()):
		g = {"group": r.u32(), "default_transition_ms": r.u32()}
		g["transitions"] = [{"from": r.u32(), "to": r.u32(), "ms": r.u32()}
							for _ in range(r.u32())]
		groups.append(g)
	d["state_groups"] = groups
	switches = []
	for _ in range(r.u32()):
		s = {"group": r.u32(), "rtpc_id": r.u32(),
			 "rtpc_type": RTPC_TYPES.get(r.u8(), "?")}
		s["points"] = [{"x": round(r.f32(), 6), "y": r.u32(),
						"interp": enum(CURVES, r.u32())}
					   for _ in range(r.u32())]
		switches.append(s)
	d["switch_groups"] = switches
	params = []
	for _ in range(r.u32()):
		params.append({"rtpc_id": r.u32(), "default": round(r.f32(), 6),
					   "ramp_type": r.u32(), "ramp_up": round(r.f32(), 6),
					   "ramp_down": round(r.f32(), 6),
					   "builtin_param": r.u8()})
	d["game_parameters"] = params
	d["unconsumed"] = r.left()
	return d


def parse_hirc(data, off, size, ctx):
	r = Reader(data, off, off + size)
	count = r.u32()
	objs = []
	for i in range(count):
		o = {"index": i, "offset": r.pos}
		try:
			t = r.u8()
			osz = r.u32()
		except ParseError as e:
			o.update(type=-1, error="truncated HIRC header: %s" % e)
			objs.append(o)
			break
		o.update(type=t, type_name=HIRC_TYPES.get(t, "Unknown(0x%02X)" % t),
				 size=osz)
		start = r.pos
		end = start + osz
		if end > r.end:
			o["error"] = "declared size runs past HIRC chunk"
			objs.append(o)
			break
		if osz >= 4:
			o["id"] = struct.unpack_from("<I", data, start)[0]
		dec = DECODERS.get(t)
		if dec and osz >= 4:
			o["decoded"] = True
			sub = Reader(data, start + 4, end)
			try:
				o.update(dec(sub, ctx))
				o["fields_ok"] = True
				o["consumed"] = sub.pos - start
				if sub.pos != end:
					o["mismatch"] = ("under-consumed: parsed %d of %d bytes, "
									 "%d left" % (sub.pos - start, osz, end - sub.pos))
					o["leftover_hex"] = data[sub.pos:min(end, sub.pos + 32)].hex()
			except (ParseError, struct.error, IndexError, RecursionError) as e:
				o["consumed"] = sub.pos - start
				o["mismatch"] = "decode error: %s" % e
		else:
			o["decoded"] = False
		r.pos = end
		objs.append(o)
	leftover = r.left()
	return objs, leftover


def parse_bank(path):
	"""Walk the chunk list with seek(); only non-DATA chunk bodies are read."""
	bank = {"file": path, "chunks": [], "mismatches": [], "warnings": []}
	ctx = {"feedback": 0}
	with open(path, "rb") as f:
		f.seek(0, 2)
		total = f.tell()
		bank["file_size"] = total
		pos = 0
		while pos + 8 <= total:
			f.seek(pos)
			tag, size = struct.unpack("<4sI", f.read(8))
			body = pos + 8
			tagname = tag.decode("latin-1")
			bank["chunks"].append({"tag": tagname, "offset": body, "size": size})
			if not bank["chunks"][:-1] and tag != b"BKHD":
				raise ParseError("%s: not a Wwise bank (first chunk is %r, not BKHD)" % (path, tag))
			if body + size > total:
				bank["warnings"].append("chunk %s runs past end of file" % tagname)
				break
			if tag == b"DATA":
				bank["data_chunk"] = {"offset": body, "size": size}
				pos = body + size
				continue
			data = f.read(size)
			handle_chunk(bank, ctx, tag, data, body)
			pos = body + size
		if pos != total:
			bank["warnings"].append("%d trailing bytes after last chunk" % (total - pos))
	for o in bank.get("hirc", []):
		if "mismatch" in o or "error" in o:
			bank["mismatches"].append({
				"type": o.get("type_name", "?"), "id": o.get("id"),
				"size": o.get("size"), "problem": o.get("mismatch") or o.get("error")})
	bank["hirc_summary"] = summarize(bank.get("hirc", []))
	return bank


def handle_chunk(bank, ctx, tag, data, body):
	size = len(data)
	if tag == b"BKHD":
		bank["header"] = parse_bkhd(data, 0, size)
		if bank["header"]["version"] != 112:
			bank["warnings"].append(
				"bank version %d: only version 112 layouts are implemented; "
				"HIRC objects are listed by type/id/size only"
				% bank["header"]["version"])
		ctx["feedback"] = bank["header"]["feedback_flag"] & 1
	elif tag == b"DIDX":
		bank["didx"] = parse_didx(data, 0, size)
	elif tag == b"STID":
		bank["stid"] = parse_stid(data, 0, size)
	elif tag == b"STMG":
		if bank.get("header", {}).get("version") == 112:
			bank["stmg"] = parse_stmg(data, 0, size)
	elif tag == b"HIRC":
		if bank.get("header", {}).get("version") != 112:
			objs, left = parse_hirc_undecoded(data, 0, size)
		else:
			objs, left = parse_hirc(data, 0, size, ctx)
		for o in objs:
			o["offset"] += body
		bank["hirc"] = objs
		if left:
			bank["warnings"].append("HIRC chunk has %d unparsed trailing bytes" % left)


def parse_hirc_undecoded(data, off, size):
	r = Reader(data, off, off + size)
	objs = []
	for i in range(r.u32()):
		t = r.u8()
		osz = r.u32()
		o = {"index": i, "offset": r.pos, "type": t, "size": osz,
			 "type_name": HIRC_TYPES.get(t, "Unknown(0x%02X)" % t),
			 "decoded": False}
		if osz >= 4:
			o["id"] = struct.unpack_from("<I", data, r.pos)[0]
		r.pos += osz
		objs.append(o)
	return objs, r.left()


def summarize(objs):
	s = {}
	for o in objs:
		name = o.get("type_name", "?")
		e = s.setdefault(name, {"count": 0, "decoded": 0, "bad": 0, "bytes": 0})
		e["count"] += 1
		e["bytes"] += o.get("size", 0)
		e["decoded"] += 1 if o.get("decoded") else 0
		e["bad"] += 1 if ("mismatch" in o or "error" in o) else 0
	return s


# ---------------------------------------------------------------------------
# id table  (state groups, states, switches, events, RTPCs, triggers)

def collect_ids(bank):
	"""Rows (kind, id, referenced_by_type, referenced_by_id, context)."""
	rows = []
	b = "%s\t%s" % (bank["file"].rsplit("/", 1)[-1].replace(".bnk", ""),
					bank["header"]["bank_id"])

	def add(kind, i, by_type, by_id, ctxs):
		rows.append((kind, i, by_type, by_id, ctxs))

	stmg = bank.get("stmg")
	if stmg:
		for g in stmg["state_groups"]:
			add("state_group", g["group"], "STMG", "-", "declared; default transition %d ms" % g["default_transition_ms"])
			for t in g["transitions"]:
				add("state", t["from"], "STMG", g["group"], "transition from, group %d" % g["group"])
				add("state", t["to"], "STMG", g["group"], "transition to, group %d" % g["group"])
		for s in stmg["switch_groups"]:
			add("switch_group", s["group"], "STMG", "-", "declared; driven by rtpc %d" % s["rtpc_id"])
			add("rtpc", s["rtpc_id"], "STMG", s["group"], "drives switch group %d" % s["group"])
			for p in s["points"]:
				add("switch", p["y"], "STMG", s["group"], "graph point x=%s group %d" % (p["x"], s["group"]))
		for p in stmg["game_parameters"]:
			add("rtpc", p["rtpc_id"], "STMG", "-", "game parameter declared; default %s" % p["default"])

	def node_refs(o, base):
		t = o["type_name"]
		for g in base.get("state_chunk", []):
			add("state_group", g["group"], t, o["id"], "state chunk")
			for s in g["states"]:
				add("state", s["state"], t, o["id"], "state chunk, group %d" % g["group"])
				add("state_instance", s["instance"], t, o["id"], "state chunk, group %d state %d" % (g["group"], s["state"]))
		for e in base.get("rtpc", []):
			add("rtpc", e["rtpc_id"], t, o["id"], "RTPC -> %s (%d points)" % (e["param"], len(e["points"])))

	def walk_tree(o, node, level, path):
		args = o["arguments"]
		if level > 0:
			g = args[level - 1]
			kind = "state" if g["group_type"] == "State" else "switch"
			add(kind, node["key"], o["type_name"], o["id"],
				"decision tree level %d, group %d (%s)" % (level, g["group"], g["group_type"]))
		for c in node.get("children", []):
			walk_tree(o, c, level + 1, path)

	for o in bank.get("hirc", []):
		t = o.get("type")
		if not o.get("decoded") or "mismatch" in o:
			continue
		if t == T_EVENT:
			add("event", o["id"], "Event", o["id"], "%d actions" % len(o["actions"]))
		elif t == T_STATE:
			add("state_instance", o["id"], "State", o["id"], "props " + json.dumps(o["props"]))
		elif t == T_ACTION:
			cls = o["class"]
			if cls == "SetState":
				add("state_group", o["state_group"], "Action", o["id"], "%s" % o["action_name"])
				add("state", o["state"], "Action", o["id"], "%s group %d" % (o["action_name"], o["state_group"]))
			elif cls == "SetSwitch":
				add("switch_group", o["switch_group"], "Action", o["id"], "%s" % o["action_name"])
				add("switch", o["switch"], "Action", o["id"], "%s group %d" % (o["action_name"], o["switch_group"]))
			elif cls == "SetGameParameter":
				add("rtpc", o["target_id"], "Action", o["id"], "%s base=%s" % (o["action_name"], o["value"]["base"]))
			elif cls in ("Event", "PlayEvent"):
				add("event", o["target_id"], "Action", o["id"], "%s targets event" % o["action_name"])
			elif cls == "Trigger":
				add("trigger", o["target_id"], "Action", o["id"], o["action_name"])
		elif t in (T_SEGMENT, T_TRACK, T_MSWITCH, T_MRANSEQ):
			node_refs(o, o["base"])
			for s in o.get("stingers", []):
				add("trigger", s["trigger_id"], o["type_name"], o["id"], "stinger -> segment %d" % s["segment_id"])
			if t == T_TRACK and "switch" in o:
				sw = o["switch"]
				kind = "state" if sw["group_type"] == "State" else "switch"
				add(kind + "_group", sw["group"], o["type_name"], o["id"], "track switch group")
				add(kind, sw["default"], o["type_name"], o["id"], "track default, group %d" % sw["group"])
				for a in sw["assoc"]:
					add(kind, a, o["type_name"], o["id"], "track switch assoc, group %d" % sw["group"])
			if t == T_MSWITCH:
				for a in o["arguments"]:
					kind = "state_group" if a["group_type"] == "State" else "switch_group"
					add(kind, a["group"], o["type_name"], o["id"], "music switch argument")
				if o["tree"]:
					walk_tree(o, o["tree"], 0, [])
	return [(b,) + r for r in rows]


def dedup_rows(rows):
	seen = set()
	out = []
	for r in rows:
		if r not in seen:
			seen.add(r)
			out.append(r)
	return out


# ---------------------------------------------------------------------------
# text output

def fmt_ms(v):
	return "%.3f ms" % v


def fmt_id(i):
	if i == ANY_NODE:
		return "ANY(-1)"
	if i == 0xFFFFFFFE:
		return "NONE(-2)"
	return str(i)


def fade_str(f):
	return "%dms/%s/off%d" % (f["time_ms"], f["curve"], f["offset_ms"])


def print_playlist(n, out, ind, index):
	seg = index.get(n["segment_id"])
	label = ("segment %d" % n["segment_id"]) if n["segment_id"] else "(group)"
	out.append("%s- item %d %s  %s loop=%s[%d..%d] weight=%d avoid_repeat=%d "
			   "use_weight=%d shuffle=%d" % (
				   ind, n["item_id"], label, n["rs_type"], n["loop"], n["loop_min"],
				   n["loop_max"], n["weight"], n["avoid_repeat"], n["use_weight"],
				   n["shuffle"]))
	for c in n["children"]:
		print_playlist(c, out, ind + "  ", index)


def print_tree_node(o, node, level, out, ind):
	args = o["arguments"]
	key = fmt_id(node["key"])
	if level == 0:
		head = "root"
	else:
		a = args[level - 1]
		head = "%s %s=%s" % (a["group_type"].lower(), a["group"], key)
	if "audio_id" in node:
		out.append("%s%s -> node %s (weight %d, prob %d)" % (ind, head, fmt_id(node["audio_id"]), node["weight"], node["prob"]))
	else:
		out.append("%s%s (weight %d, prob %d)" % (ind, head, node["weight"], node["prob"]))
		for c in node["children"]:
			print_tree_node(o, c, level + 1, out, ind + "  ")


def describe(o, index, out):
	t = o["type"]
	head = "[%s] id=%s size=%d" % (o["type_name"], o.get("id"), o["size"])
	if "mismatch" in o:
		head += "  **MISMATCH: %s**" % o["mismatch"]
	out.append(head)
	if not o.get("fields_ok"):
		return
	ind = "    "
	if t == T_STATE:
		out.append("%sprops: %s" % (ind, o.get("props")))
	elif t == T_EVENT:
		out.append("%sactions: %s" % (ind, o.get("actions")))
	elif t == T_ACTION:
		bits = ["%s (%s)" % (o["action_name"], o["action_type"]),
				"target=%s%s" % (o["target_id"], " bus" if o.get("target_is_bus") else "")]
		for k in ("state_group", "state", "switch_group", "switch", "bank_id", "fade_curve", "value", "except"):
			if k in o and o[k] not in ([], None):
				bits.append("%s=%s" % (k, o[k]))
		if o["props"]:
			bits.append("props=%s" % o["props"])
		out.append(ind + " ".join(bits))
	elif t in (T_SEGMENT, T_TRACK, T_MSWITCH, T_MRANSEQ):
		base = o.get("base", {})
		if t != T_TRACK:
			m = o["meter"]
			out.append("%sparent=%s children=%s" % (ind, base.get("parent"), o["children"]))
			out.append("%smeter: %.2f bpm %d/%d grid=%s/%s ms override=%d" % (
				ind, m["tempo_bpm"], m["beats_per_bar"], m["beat_value"],
				m["grid_period_ms"], m["grid_offset_ms"], o["meter_override"]))
		else:
			out.append("%sparent=%s type=%s look_ahead=%dms" % (ind, base.get("parent"), o["track_type"], o["look_ahead_ms"]))
		if base.get("props"):
			out.append("%sprops: %s" % (ind, base["props"]))
		if base.get("state_chunk"):
			out.append("%sstate chunk: %s" % (ind, json.dumps(base["state_chunk"])))
		if base.get("rtpc"):
			out.append("%srtpc: %s" % (ind, json.dumps(base["rtpc"])))
		for s in o.get("stingers", []):
			out.append("%sstinger %s" % (ind, s))
		if t == T_SEGMENT:
			out.append("%sduration: %s" % (ind, fmt_ms(o["duration_ms"])))
			for m in o["markers"]:
				out.append("%scue id=%d %s at %s %s" % (ind, m["id"], m.get("role", ""), fmt_ms(m["pos_ms"]), m["name"]))
		if t == T_TRACK:
			for s in o["sources"]:
				out.append("%ssource media=%d %s plugin=%s in_mem=%d" % (ind, s["source_id"], s["stream"], s["plugin"], s["in_memory_size"]))
			for c in o["clips"]:
				out.append("%sclip idx=%d media=%d at %s trim[%s..%s] dur=%s" % (
					ind, c["track_index"], c["source_id"], fmt_ms(c["play_at_ms"]),
					c["begin_trim_ms"], c["end_trim_ms"], fmt_ms(c["src_duration_ms"])))
			for a in o["clip_automation"]:
				out.append("%sclip automation %s" % (ind, a))
			if "switch" in o:
				out.append("%sswitch %s trans %s" % (ind, o["switch"], o["trans"]))
		if t in (T_MSWITCH, T_MRANSEQ):
			for i, ru in enumerate(o["rules"]):
				out.append("%srule %d: %s -> %s | %s" % (ind, i, [fmt_id(x) for x in ru["src"]], [fmt_id(x) for x in ru["dst"]], rule_text(ru)))
		if t == T_MSWITCH:
			out.append("%scontinue_playback=%d depth=%d mode=%s arguments=%s" % (
				ind, o["continue_playback"], o["tree_depth"], o["mode"], o["arguments"]))
			if o["tree"]:
				print_tree_node(o, o["tree"], 0, out, ind + "  ")
		if t == T_MRANSEQ:
			print_playlist(o["playlist"], out, ind + "  ", index)


def rule_text(ru):
	s, d = ru["src_rule"], ru["dst_rule"]
	txt = "exit: sync=%s fade=%s post_exit=%d; enter: entry=%s fade=%s pre_entry=%d jump_to=%s" % (
		s["sync"], fade_str(s["fade"]), s["play_post_exit"], d["entry"],
		fade_str(d["fade"]), d["play_pre_entry"], fmt_id(d["jump_to"]))
	if "transition_object" in ru:
		to = ru["transition_object"]
		txt += "; via segment %d (fade in %s out %s)" % (to["segment_id"], fade_str(to["fade_in"]), fade_str(to["fade_out"]))
	return txt


def dump_text(bank, out):
	h = bank.get("header", {})
	out.append("== %s (%d bytes)" % (bank["file"], bank["file_size"]))
	out.append("BKHD: %s" % h)
	out.append("chunks: " + ", ".join("%s(%d)" % (c["tag"], c["size"]) for c in bank["chunks"]))
	if "didx" in bank:
		out.append("DIDX: %d media entries, data chunk %s" % (len(bank["didx"]), bank.get("data_chunk")))
		for e in bank["didx"]:
			out.append("    media %d offset=%d size=%d" % (e["id"], e["offset"], e["size"]))
	if "stid" in bank:
		out.append("STID: %s" % bank["stid"]["entries"])
	if "stmg" in bank:
		s = bank["stmg"]
		out.append("STMG: %d state groups, %d switch groups, %d game parameters" % (
			len(s["state_groups"]), len(s["switch_groups"]), len(s["game_parameters"])))
		for g in s["state_groups"]:
			out.append("    state group %d default_transition=%dms transitions=%d" % (g["group"], g["default_transition_ms"], len(g["transitions"])))
		for g in s["switch_groups"]:
			out.append("    switch group %d rtpc=%d points=%s" % (g["group"], g["rtpc_id"], g["points"]))
		for p in s["game_parameters"]:
			out.append("    game parameter %d default=%s ramp=%d up=%s down=%s" % (p["rtpc_id"], p["default"], p["ramp_type"], p["ramp_up"], p["ramp_down"]))
	out.append("HIRC summary (count/decoded/bad/bytes):")
	for k, v in sorted(bank["hirc_summary"].items()):
		out.append("    %-28s %5d %5d %4d %8d" % (k, v["count"], v["decoded"], v["bad"], v["bytes"]))
	index = {o["id"]: o for o in bank.get("hirc", []) if "id" in o}
	for o in bank.get("hirc", []):
		if "type_name" in o:
			describe(o, index, out)
		else:
			out.append("HIRC error: %s" % o.get("error"))
	for w in bank["warnings"]:
		out.append("WARNING: " + w)
	if bank["mismatches"]:
		out.append("SIZE MISMATCHES (%d):" % len(bank["mismatches"]))
		for m in bank["mismatches"]:
			out.append("    %s" % m)
	else:
		out.append("size check: every HIRC object consumed exactly its declared size")


def dump_tree(bank, out, others=()):
	"""Music hierarchy: roots (objects nobody lists as child) down to media.
	`others` are the other banks given on the command line; they are only used
	to say where an object that is missing from this bank lives."""
	index = {o["id"]: o for o in bank.get("hirc", []) if "id" in o}
	elsewhere = {}
	for ob in others:
		for o in ob.get("hirc", []):
			if o.get("type") in (T_SEGMENT, T_MSWITCH, T_MRANSEQ, T_TRACK) and "id" in o:
				elsewhere.setdefault(o["id"], []).append(
					ob["file"].rsplit("/", 1)[-1].replace(".bnk", ""))
	media = {e["id"]: e for e in bank.get("didx", [])}
	referenced = set()
	for o in bank.get("hirc", []):
		if o.get("type") in (T_SEGMENT, T_MSWITCH, T_MRANSEQ) and o.get("decoded") and "children" in o:
			referenced.update(o["children"])
	roots = [o for o in bank.get("hirc", []) if o.get("type") in (T_MSWITCH, T_MRANSEQ, T_SEGMENT) and o.get("decoded") and o["id"] not in referenced]
	out.append("== %s  bank_id=%s" % (bank["file"], bank.get("header", {}).get("bank_id")))

	def missing(kind, nid, ind):
		if nid == 0:
			out.append("%s(no node: silence)" % ind)
		elif nid in elsewhere:
			out.append("%s%s %d  (not in this bank; defined in %s)" % (ind, kind, nid, ", ".join(elsewhere[nid])))
		else:
			out.append("%s%s %d  (not in this bank; not found in the other banks given)" % (ind, kind, nid))

	def seg_lines(sid, ind, lead=""):
		s = index.get(sid)
		if not s:
			missing("segment", sid, ind + lead)
			return
		cues = ", ".join("%s@%.0fms" % (m.get("role") or m["id"], m["pos_ms"]) for m in s.get("markers", []))
		out.append("%s%ssegment %d  %.0f ms  %.1f bpm %d/%d  cues: %s%s" % (
			ind, lead, sid, s["duration_ms"], s["meter"]["tempo_bpm"], s["meter"]["beats_per_bar"],
			s["meter"]["beat_value"], cues, "" if s["children"] else "  ** NO TRACKS (silent) **"))
		for tid in s["children"]:
			t = index.get(tid)
			if not t:
				missing("track", tid, ind + "  ")
				continue
			srcs = []
			for src in t["sources"]:
				m = media.get(src["source_id"])
				srcs.append("media %d%s" % (src["source_id"], " (%d bytes)" % m["size"] if m else " (not in DIDX)"))
			sw = ""
			if "switch" in t:
				sw = " switch(%s group %d default %d assoc %s)" % (t["switch"]["group_type"], t["switch"]["group"], t["switch"]["default"], t["switch"]["assoc"])
			out.append("%s  track %d [%s]%s -> %s" % (ind, tid, t["track_type"], sw, ", ".join(srcs)))

	def playlist_lines(n, ind):
		label = ("segment %d" % n["segment_id"]) if n["segment_id"] else "group"
		out.append("%s- item %d %s: %s loop=%s[%d..%d] weight=%d avoid_repeat=%d use_weight=%d shuffle=%d" % (
			ind, n["item_id"], label, n["rs_type"], n["loop"], n["loop_min"], n["loop_max"],
			n["weight"], n["avoid_repeat"], n["use_weight"], n["shuffle"]))
		if n["segment_id"]:
			seg_lines(n["segment_id"], ind + "    ")
		for c in n["children"]:
			playlist_lines(c, ind + "  ")

	def node_lines(nid, ind):
		o = index.get(nid)
		if o is None:
			missing("node", nid, ind)
			return
		if o["type"] == T_SEGMENT:
			seg_lines(nid, ind)
		elif o["type"] == T_MRANSEQ:
			out.append("%smusic random/sequence container %d  children=%d" % (ind, nid, len(o["children"])))
			playlist_lines(o["playlist"], ind + "  ")
			used = set()
			stack = [o["playlist"]]
			while stack:
				n = stack.pop()
				used.add(n["segment_id"])
				stack.extend(n["children"])
			for sid in o["children"]:
				if sid not in used:
					seg_lines(sid, ind + "  ", "(child not in playlist) ")
		elif o["type"] == T_MSWITCH:
			out.append("%smusic switch %d  mode=%s continue_playback=%d args=%s" % (ind, nid, o["mode"], o["continue_playback"], o["arguments"]))
			if o["tree"]:
				walk_switch(o, o["tree"], 0, ind + "  ")
			for i, ru in enumerate(o["rules"]):
				out.append("%s  transition rule %d: %s -> %s | %s" % (ind, i, [fmt_id(x) for x in ru["src"]], [fmt_id(x) for x in ru["dst"]], rule_text(ru)))

	def walk_switch(o, node, level, ind):
		args = o["arguments"]
		if level > 0:
			a = args[level - 1]
			label = "%s %d = %s" % (a["group_type"], a["group"], fmt_id(node["key"]))
		else:
			label = "root"
		if "audio_id" in node:
			out.append("%s%s -> (w%d p%d)" % (ind, label, node["weight"], node["prob"]))
			node_lines(node["audio_id"], ind + "  ")
		else:
			out.append("%s%s (w%d p%d)" % (ind, label, node["weight"], node["prob"]))
			for c in node["children"]:
				walk_switch(o, c, level + 1, ind + "  ")

	for o in roots:
		node_lines(o["id"], "")


def main(argv):
	flags = [a for a in argv if a.startswith("--")]
	paths = [a for a in argv if not a.startswith("--")]
	if not paths or any(f not in ("--json", "--tree", "--ids") for f in flags):
		sys.stderr.write(__doc__)
		return 2
	try:
		banks = [parse_bank(p) for p in paths]
	except (OSError, struct.error, ParseError) as e:
		sys.stderr.write("error: %s\n" % e)
		return 2
	if "--json" in flags:
		json.dump(banks if len(banks) > 1 else banks[0], sys.stdout, indent=1)
		print()
	elif "--ids" in flags:
		print("bank\tbank_id\tkind\tid\treferenced_by_type\treferenced_by_id\tcontext")
		for bank in banks:
			for row in dedup_rows(collect_ids(bank)):
				print("\t".join(str(c) for c in row))
	else:
		out = []
		for bank in banks:
			if "--tree" in flags:
				dump_tree(bank, out, [b for b in banks if b is not bank])
			else:
				dump_text(bank, out)
		print("\n".join(out))
	return 1 if any(b["mismatches"] for b in banks) else 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))

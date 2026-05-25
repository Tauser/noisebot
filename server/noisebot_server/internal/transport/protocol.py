"""NoiseBot bridge protocol facade.

The protocol remains byte-owned by ``bridge_v2`` for this phase. Server code
imports from here so future movement is isolated to this module.
"""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.protocol.codec import FrameDecoder
from bridgev2.protocol.framing import (
    FRAME_OVERHEAD,
    MAX_FRAME_DATA_LEN,
    SOF,
    crc8,
    decode_frames,
    encode_frame,
)
from bridgev2.protocol.messages import (
    BRIDGE_V2_HELLO_CAPABILITIES,
    MSG_ACTION,
    MSG_AUDIO_CHUNK,
    MSG_EMOT_EVENT,
    MSG_EVENT,
    MSG_EXPR,
    MSG_GAZE,
    MSG_HELLO,
    MSG_SAY,
    MSG_SAY_BEGIN,
    MSG_SAY_END,
    MSG_SESSION,
    MSG_SPEECH_CANCEL,
    MSG_STATUS,
    MSG_TEXT_SCROLL,
    MSG_VOLUME,
    NB_EVT_VOICE_ACTIVITY_END,
    NB_EVT_VOICE_ACTIVITY_START,
    PROTOCOL_NAME,
    PROTOCOL_VERSION,
    decode_event,
    decode_hello,
    decode_session,
    decode_status,
    encode_action,
    encode_emot_event,
    encode_expr,
    encode_gaze,
    encode_hello,
    encode_say_begin,
    encode_say_end,
    encode_session,
    encode_speech_cancel,
    encode_text_scroll,
    encode_volume,
)

__all__ = [
    "BRIDGE_V2_HELLO_CAPABILITIES",
    "FRAME_OVERHEAD",
    "FrameDecoder",
    "MAX_FRAME_DATA_LEN",
    "MSG_ACTION",
    "MSG_AUDIO_CHUNK",
    "MSG_EMOT_EVENT",
    "MSG_EVENT",
    "MSG_EXPR",
    "MSG_GAZE",
    "MSG_HELLO",
    "MSG_SAY",
    "MSG_SAY_BEGIN",
    "MSG_SAY_END",
    "MSG_SESSION",
    "MSG_SPEECH_CANCEL",
    "MSG_STATUS",
    "MSG_TEXT_SCROLL",
    "MSG_VOLUME",
    "NB_EVT_VOICE_ACTIVITY_END",
    "NB_EVT_VOICE_ACTIVITY_START",
    "PROTOCOL_NAME",
    "PROTOCOL_VERSION",
    "SOF",
    "crc8",
    "decode_event",
    "decode_frames",
    "decode_hello",
    "decode_session",
    "decode_status",
    "encode_action",
    "encode_emot_event",
    "encode_expr",
    "encode_frame",
    "encode_gaze",
    "encode_hello",
    "encode_say_begin",
    "encode_say_end",
    "encode_session",
    "encode_speech_cancel",
    "encode_text_scroll",
    "encode_volume",
]

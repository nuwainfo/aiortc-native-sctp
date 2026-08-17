#!/usr/bin/env python
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# FastFileLink CLI - Fast, no-fuss file sharing
# Copyright (C) 2025-2026 FastFileLink contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""aiortc RTCDataChannel glue backed by native usrsctp.

aiortc continues to own RTCDataChannel / DCEP and DTLS / ICE.  This module
replaces the pure-Python SCTP association with the bundled ``_native_sctp``
extension.
"""

from __future__ import annotations

import asyncio
import logging
import time
import weakref
from collections import deque
from typing import Deque, Optional

import aiortc
import aiortc.rtcpeerconnection as rtcpeerconnection
import aiortc.rtcsctptransport as rtcsctptransport
from aiortc.rtcdatachannel import RTCDataChannel
from aiortc.rtcsctptransport import (
    RTCSctpCapabilities,
    RTCSctpTransport,
    WEBRTC_DCEP,
)

from . import _native_sctp

logger = logging.getLogger(__name__)

# Proven defaults used by the native WebRTC SCTP path.
# MTU is the complete SCTP packet ceiling before DTLS / UDP / IP overhead.
NATIVE_SCTP_MTU = int(os.environ.get("NATIVE_SCTP_MTU", "1200"))
NATIVE_SCTP_SENDSPACE = int(os.environ.get("NATIVE_SCTP_SENDSPACE", str(4 * 1024 * 1024)))
NATIVE_SCTP_RECVSPACE = int(os.environ.get("NATIVE_SCTP_RECVSPACE", str(4 * 1024 * 1024)))
NATIVE_SCTP_SACK_FREQ = int(os.environ.get("NATIVE_SCTP_SACK_FREQ", "2"))
NATIVE_SCTP_DELAYED_SACK_MS = int(os.environ.get("NATIVE_SCTP_DELAYED_SACK_MS", "20"))
NATIVE_SCTP_TIMER_MS = max(1, int(os.environ.get("NATIVE_SCTP_TIMER_MS", "10")))

_native_sctp.configure(
    sendspace=NATIVE_SCTP_SENDSPACE,
    recvspace=NATIVE_SCTP_RECVSPACE,
    sack_freq=NATIVE_SCTP_SACK_FREQ,
    delayed_sack_ms=NATIVE_SCTP_DELAYED_SACK_MS,
)

_instances: "weakref.WeakSet[NativeRTCSctpTransport]" = weakref.WeakSet()
_timer_task: Optional[asyncio.Task] = None
_timer_loop: Optional[asyncio.AbstractEventLoop] = None


async def _global_timer_loop() -> None:
    """Drive usrsctp's timer wheel without a native timer thread."""
    global _timer_task, _timer_loop

    last = time.monotonic()
    try:
        while _instances:
            await asyncio.sleep(NATIVE_SCTP_TIMER_MS / 1000.0)
            now = time.monotonic()
            elapsed_ms = max(1, int((now - last) * 1000.0))
            last = now

            _native_sctp.tick(elapsed_ms)
            # Timer expiry can generate raw SCTP packets / notifications.
            for transport in list(_instances):
                await transport._native_pump()
    finally:
        _timer_task = None
        _timer_loop = None


def _ensure_timer() -> None:
    global _timer_task, _timer_loop
    loop = asyncio.get_running_loop()
    if _timer_task is None or _timer_task.done():
        _timer_loop = loop
        _timer_task = loop.create_task(_global_timer_loop())
    elif _timer_loop is not loop:
        raise RuntimeError("Native usrsctp transports must share one asyncio event loop")


class NativeRTCSctpTransport(RTCSctpTransport):
    """Drop-in RTCSctpTransport using usrsctp for the SCTP association.

    aiortc continues to own RTCDataChannel / DCEP objects while usrsctp owns
    the SCTP wire protocol and congestion control.
    """

    def __init__(self, transport, port: int = 5000) -> None:
        super().__init__(transport, port=port)
        self._native_assoc: Optional[_native_sctp.Association] = None
        self._native_pumping = False
        self._native_flush_running = False
        self._native_flush_pending = False
        self._native_send_offset = 0
        self._native_reset_pending: Deque[int] = deque()
        self._native_reset_flags: dict[int, int] = {}

    async def start(self, remoteCaps: RTCSctpCapabilities, remotePort: int) -> None:
        # Match aiortc 1.15.0's public start() state / channel-ID behavior.
        if self._RTCSctpTransport__started:
            return

        self._RTCSctpTransport__started = True
        self._RTCSctpTransport__state = "connecting"
        self._remote_port = remotePort
        self._data_channel_id = 0 if self.is_server else 1

        # aiortc's DTLS transport dispatches decrypted application_data here.
        self.transport._register_data_receiver(self)

        self._native_assoc = _native_sctp.Association(
            local_port=self._local_port,
            remote_port=remotePort,
            mtu=NATIVE_SCTP_MTU,
            outbound_streams=65535,
            inbound_streams=65535,
        )
        _instances.add(self)
        _ensure_timer()

        # WebRTC/usrsctp implementations allow both peers to call connect().
        self._native_assoc.connect()
        await self._native_pump()

    async def stop(self) -> None:
        assoc = self._native_assoc
        self._native_assoc = None
        _instances.discard(self)

        if assoc is not None:
            assoc.close()

        try:
            self.transport._unregister_data_receiver(self)
        except Exception:
            pass

        self._set_state(self.State.CLOSED)

    async def _handle_data(self, data: bytes) -> None:
        """Receive one decrypted SCTP datagram from RTCDtlsTransport."""
        assoc = self._native_assoc
        if assoc is None:
            return
        assoc.feed(data)
        await self._native_pump()

    async def _native_pump(self) -> None:
        """Move packets/messages between usrsctp, DTLS and aiortc DataChannels."""
        if self._native_pumping:
            return
        assoc = self._native_assoc
        if assoc is None:
            return

        self._native_pumping = True
        try:
            # Keep draining until no callback-generated work remains.  Sending
            # a DCEP ACK can synchronously make usrsctp generate more packets.
            for _ in range(64):
                work = assoc.poll()
                outbound = work["outbound"]
                messages = work["messages"]
                events = work["events"]
                writable = work["writable"]

                for packet in outbound:
                    await self.transport._send_data(packet)

                for event in events:
                    kind = event[0]
                    if kind == "assoc":
                        await self._native_assoc_event(*event[1:])
                    elif kind == "reset":
                        await self._native_reset_event(*event[1:])
                    elif kind == "shutdown":
                        self._set_state(self.State.CLOSED)
                    elif kind == "send_failed":
                        logger.warning("usrsctp SEND_FAILED event: %r", event[1:])

                for stream_id, pp_id, payload in messages:
                    await self._data_channel_receive(stream_id, pp_id, payload)

                if writable:
                    await self._data_channel_flush()

                if not outbound and not messages and not events:
                    break
        finally:
            self._native_pumping = False

    async def _native_assoc_event(
        self, state: int, outbound_streams: int, inbound_streams: int
    ) -> None:
        # usrsctp SCTP_ASSOC_CHANGE states from usrsctp.h:
        # 1 COMM_UP, 2 COMM_LOST, 3 RESTART, 4 SHUTDOWN_COMP, 5 CANT_STR_ASSOC.
        if state in (1, 3):
            self._outbound_streams_count = int(outbound_streams)
            self._inbound_streams_count = int(inbound_streams)
            if self._association_state != self.State.ESTABLISHED:
                self._set_state(self.State.ESTABLISHED)
            await self._data_channel_flush()
        elif state in (2, 4, 5):
            self._set_state(self.State.CLOSED)

    async def _native_reset_event(self, flags: int, stream_ids: list[int]) -> None:
        # usrsctp event flags: incoming=0x0001, outgoing=0x0002.
        incoming = bool(flags & 0x0001)
        outgoing = bool(flags & 0x0002)

        for stream_id in stream_ids:
            accumulated = self._native_reset_flags.get(stream_id, 0) | flags
            self._native_reset_flags[stream_id] = accumulated

            # If the peer reset its outgoing side, reset ours too.  This mirrors
            # aiortc's bidirectional DataChannel close semantics.
            if incoming and not outgoing and stream_id in self._data_channels:
                self._queue_native_reset(stream_id)

            if (accumulated & 0x0003) == 0x0003:
                self._native_reset_flags.pop(stream_id, None)
                if stream_id in self._data_channels:
                    self._data_channel_closed(stream_id)

        self._try_native_resets()

    def _queue_native_reset(self, stream_id: int) -> None:
        if stream_id not in self._native_reset_pending:
            self._native_reset_pending.append(stream_id)
        self._try_native_resets()

    def _try_native_resets(self) -> None:
        assoc = self._native_assoc
        if assoc is None:
            return
        while self._native_reset_pending:
            stream_id = self._native_reset_pending[0]
            if not assoc.reset_stream(stream_id):
                return
            self._native_reset_pending.popleft()

    def _data_channel_close(self, channel: RTCDataChannel) -> None:
        if channel.readyState in ("closing", "closed"):
            return

        channel._setReadyState("closing")
        stream_id = channel.id
        if stream_id is None:
            return

        if self._association_state == self.State.ESTABLISHED:
            self._queue_native_reset(stream_id)
        else:
            self._data_channels.pop(stream_id, None)
            channel._setReadyState("closed")

    async def _data_channel_flush(self) -> None:
        """Flush aiortc's DataChannel queue into usrsctp, without Python SCTP."""
        if self._native_flush_running:
            self._native_flush_pending = True
            return
        if self._association_state != self.State.ESTABLISHED:
            return

        assoc = self._native_assoc
        if assoc is None:
            return

        self._native_flush_running = True
        try:
            while True:
                self._native_flush_pending = False

                while self._data_channel_queue:
                    channel, protocol, user_data = self._data_channel_queue[0]

                    stream_id = channel.id
                    if stream_id is None:
                        stream_id = self._data_channel_id
                        while stream_id in self._data_channels:
                            stream_id += 2
                        self._data_channels[stream_id] = channel
                        channel._setId(stream_id)

                    max_retransmits = None
                    max_lifetime_ms = None
                    ordered = True
                    if protocol != WEBRTC_DCEP:
                        ordered = channel.ordered
                        max_retransmits = channel.maxRetransmits
                        max_lifetime_ms = channel.maxPacketLifeTime

                    # SCTP_EXPLICIT_EOR makes usrsctp_sendv() non-atomic: a
                    # non-blocking call may accept only a prefix of a DataChannel
                    # message.  Keep the queue head and resume at the accepted
                    # byte offset until the complete user message has entered
                    # usrsctp.  Starting the next queue item early would merge
                    # two same-SID / same-PPID messages on the wire.
                    offset = self._native_send_offset
                    remaining = memoryview(user_data)[offset:]
                    accepted_raw = assoc.send(
                        stream_id=stream_id,
                        ppid=protocol,
                        data=remaining,
                        ordered=ordered,
                        max_retransmits=max_retransmits,
                        max_lifetime_ms=max_lifetime_ms,
                    )
                    if isinstance(accepted_raw, bool):
                        raise RuntimeError(
                            "_native_sctp Association.send() must return a byte count, not bool"
                        )
                    accepted = int(accepted_raw)
                    if accepted <= 0:
                        # Native send buffer is full. send_cb / timer processing
                        # will mark it writable and call us again through pump().
                        break
                    if accepted > len(remaining):
                        raise RuntimeError(
                            f"usrsctp accepted {accepted} bytes from a {len(remaining)}-byte buffer"
                        )

                    self._native_send_offset += accepted
                    if protocol != WEBRTC_DCEP:
                        channel._addBufferedAmount(-accepted)

                    if self._native_send_offset < len(user_data):
                        # A short positive write is normal with explicit EOR.
                        # Pump the packets accepted so far before retrying the
                        # remaining suffix of this exact same user message.
                        work = assoc.poll()
                        for packet in work["outbound"]:
                            await self.transport._send_data(packet)
                        for event in work["events"]:
                            kind = event[0]
                            if kind == "assoc":
                                await self._native_assoc_event(*event[1:])
                            elif kind == "reset":
                                await self._native_reset_event(*event[1:])
                        for sid, pp_id, payload in work["messages"]:
                            await self._data_channel_receive(sid, pp_id, payload)
                        # Retry this same queue head once more before yielding.
                        # If the send buffer is still full, the next send()
                        # returns 0 and normal writable/timer processing resumes.
                        self._native_flush_pending = True
                        break

                    self._native_send_offset = 0
                    self._data_channel_queue.popleft()

                    # Pull freshly-created raw SCTP packets quickly instead of
                    # waiting for the next 10 ms timer tick.
                    work = assoc.poll()
                    for packet in work["outbound"]:
                        await self.transport._send_data(packet)
                    for event in work["events"]:
                        kind = event[0]
                        if kind == "assoc":
                            await self._native_assoc_event(*event[1:])
                        elif kind == "reset":
                            await self._native_reset_event(*event[1:])
                    for sid, pp_id, payload in work["messages"]:
                        await self._data_channel_receive(sid, pp_id, payload)

                if not self._native_flush_pending:
                    break
        finally:
            self._native_flush_running = False



def install_native_sctp() -> None:
    """Make future aiortc RTCPeerConnection instances use native usrsctp."""
    rtcpeerconnection.RTCSctpTransport = NativeRTCSctpTransport
    rtcsctptransport.RTCSctpTransport = NativeRTCSctpTransport
    aiortc.RTCSctpTransport = NativeRTCSctpTransport

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

"""Loopback smoke test for _native_sctp without aiortc / DTLS.

Creates two AF_CONN associations in the same process and manually wires each
association's raw SCTP output into the other one's conninput().  This validates
the C extension / usrsctp build before testing WebRTC.
"""

import time

from . import _native_sctp


def pump(a, b, *, timeout=5.0):
    deadline = time.monotonic() + timeout
    states = {"a": None, "b": None}
    messages = []
    while time.monotonic() < deadline:
        _native_sctp.tick(10)
        progressed = False
        for name, src, dst in (("a", a, b), ("b", b, a)):
            work = src.poll()
            for packet in work["outbound"]:
                dst.feed(packet)
                progressed = True
            for event in work["events"]:
                if event[0] == "assoc":
                    states[name] = event[1]
                    progressed = True
            for message in work["messages"]:
                messages.append((name, message))
                progressed = True
        if states["a"] in (1, 3) and states["b"] in (1, 3):
            return states, messages
        if not progressed:
            time.sleep(0.001)
    raise RuntimeError(f"association did not establish: {states}")


def main():
    _native_sctp.configure(
        sendspace=4 * 1024 * 1024,
        recvspace=4 * 1024 * 1024,
        sack_freq=2,
        delayed_sack_ms=20,
    )
    a = _native_sctp.Association(local_port=5000, remote_port=5000, mtu=1200)
    b = _native_sctp.Association(local_port=5000, remote_port=5000, mtu=1200)
    try:
        a.connect()
        b.connect()
        states, _ = pump(a, b)
        print("association up:", states)

        payload = (b"native-usrsctp-smoke-" * 20000)[:256 * 1024]
        accepted_raw = a.send(stream_id=1, ppid=53, data=payload, ordered=True)
        if isinstance(accepted_raw, bool):
            raise RuntimeError(
                "_native_sctp Association.send() must return a byte count, not bool"
            )
        accepted = int(accepted_raw)
        if accepted != len(payload):
            raise RuntimeError(
                f"initial send accepted {accepted}/{len(payload)} bytes; "
                "smoke expects an empty send buffer to accept the whole message"
            )

        deadline = time.monotonic() + 5.0
        got = None
        while time.monotonic() < deadline and got is None:
            _native_sctp.tick(10)
            # poll() returns messages received by *src*.  The original smoke
            # test accidentally labelled src with dst's name, so a correctly
            # delivered A -> B message was ignored when it appeared in b.poll().
            for receiver_name, src, dst in (("a", a, b), ("b", b, a)):
                work = src.poll()
                for packet in work["outbound"]:
                    dst.feed(packet)
                for event in work["events"]:
                    print(f"{receiver_name} event:", event)
                for sid, ppid, data in work["messages"]:
                    print(
                        f"{receiver_name} message:",
                        len(data), "bytes", "sid=", sid, "ppid=", ppid,
                    )
                    if receiver_name == "b":
                        got = (sid, ppid, data)
            if got is None:
                time.sleep(0.001)

        if got is None:
            print("A status at timeout:", a.status())
            print("B status at timeout:", b.status())
            raise RuntimeError("message was not delivered")
        sid, ppid, data = got
        assert sid == 1, sid
        assert ppid == 53, ppid
        assert data == payload, (len(data), len(payload))
        print("message OK:", len(data), "bytes", "sid=", sid, "ppid=", ppid)
        print("A status:", a.status())
        print("B status:", b.status())
    finally:
        a.close()
        b.close()


if __name__ == "__main__":
    main()

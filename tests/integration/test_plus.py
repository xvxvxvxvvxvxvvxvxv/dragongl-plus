#!/usr/bin/env python3
# DragonGL+ - Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
"""The protocol extension of DragonGL+ (include/protocol_plus.h) on a real
server: a client that offers "dragongl-plus/5.1" gets it, with the
inventory, spellbook, progress and the identity of the entities it sees;
a Dragon GL client (only "dragongl/5") never receives a frame of the
extension (dglproto's strict decoder would raise on one)."""

import os
import shutil
import ssl
import struct
import tempfile
import time
import unittest

import dglproto
from test_server import SERVER_PASS, ServerProcess, free_port

ALPN_PLUS = "dragongl-plus/5.1"
MSG_PLUS_ENTITY_INFO = 256
MSG_PLUS_PLAYER_EXT = 257
MSG_PLUS_INVENTORY = 258
MSG_PLUS_SPELLBOOK = 260
KIND_PLAYER = 20


class PlusClient(dglproto.Client):
    """A client that offers the extension first, like the DragonGL+ client.
    Frames of the extension are counted (and the entity infos kept); the
    others go through dglproto's decoder."""

    def __init__(self, port):
        self.plus_frames = {}
        self.entity_infos = []
        orig = dglproto.tls_context

        def both(alpn=None):
            ctx = orig(None)
            ctx.set_alpn_protocols([ALPN_PLUS, dglproto.ALPN])
            return ctx

        dglproto.tls_context = both
        try:
            super().__init__(port)
        finally:
            dglproto.tls_context = orig

    def _decode(self):
        while len(self._buf) >= dglproto.HEADER.size:
            version, msg_type, length = dglproto.HEADER.unpack_from(self._buf)
            if len(self._buf) < dglproto.HEADER.size + length:
                return
            if msg_type < MSG_PLUS_ENTITY_INFO:
                # one frame through the strict decoder of the base protocol
                rest = self._buf[dglproto.HEADER.size + length:]
                self._buf = self._buf[:dglproto.HEADER.size + length]
                super()._decode()
                self._buf = rest + self._buf
                continue
            payload = self._buf[dglproto.HEADER.size:dglproto.HEADER.size + length]
            self._buf = self._buf[dglproto.HEADER.size + length:]
            self.plus_frames[msg_type] = self.plus_frames.get(msg_type, 0) + 1
            if msg_type == MSG_PLUS_ENTITY_INFO:
                entity_id, kind, _arch, level = struct.unpack_from("<iBBB", payload)
                name_len = payload[13]
                name = payload[14:14 + name_len].decode(errors="replace")
                self.entity_infos.append((entity_id, kind, level, name))


class PlusTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="dragongl-plus-it-")
        cls.server = ServerProcess(cls.root, free_port())
        cls.server.wait_ready(timeout=600)

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        shutil.rmtree(cls.root, ignore_errors=True)

    def login(self, client, name):
        client.login(name, "pw-%s" % name, SERVER_PASS)
        self.assertTrue(client.wait_for_login(), "login of %r failed: %r" % (name, client.auth_fail))

    def test_plus_client_gets_the_extension(self):
        c = PlusClient(self.server.port)
        try:
            self.assertEqual(c.alpn, ALPN_PLUS)
            self.login(c, "plusone")
            c.pump(1.5)
            for t in (MSG_PLUS_INVENTORY, MSG_PLUS_SPELLBOOK, MSG_PLUS_PLAYER_EXT):
                self.assertGreaterEqual(c.plus_frames.get(t, 0), 1, "no frame of type %d" % t)
            # a second player in the square: the first one learns who it is
            other = dglproto.Client(self.server.port)
            try:
                self.login(other, "plaintwo")
                c.pump(1.5)
                infos = [i for i in c.entity_infos if i[0] == other.entity_id]
                self.assertTrue(infos, "no identity for the other player: %r" % c.entity_infos)
                self.assertEqual(infos[-1][1], KIND_PLAYER)
                self.assertEqual(infos[-1][3], "plaintwo")
            finally:
                other.close()
        finally:
            c.close()

    def test_dragon_gl_client_sees_plain_v5(self):
        c = dglproto.Client(self.server.port)
        try:
            self.assertEqual(c.alpn, dglproto.ALPN)
            self.login(c, "classic")
            c.command("inventory")
            # every frame goes through the strict v5 decoder: a frame of
            # the extension would raise here
            self.assertIsNotNone(c.wait_for_text("INVENTORY", 5))
            c.pump(1.0)
        finally:
            c.close()


if __name__ == "__main__":
    unittest.main()

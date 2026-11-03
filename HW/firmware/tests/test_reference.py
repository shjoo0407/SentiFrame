import binascii
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
FW = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(FW / 'tools'))
from pi_receiver import Packet, Decoder, Capture, CaptureError, interpret, HELLO, IMU, EVENT, HISTORY, STATUS

class ReferenceTests(unittest.TestCase):
    def test_hello_feature_combinations(self):
        for mask in range(32):
            data = interpret(Packet(HELLO,1,0,1,bytes([1,6,0,2,0,mask])))
            self.assertEqual(data['firmware'], '0.2.0')
            for name, bit in [('gyro',1),('ppg',2),('pack_approved',4),('imu',8),
                              ('charging_requested',16)]:
                self.assertEqual(data['features'][name], bool(mask & bit))
    def test_legacy_hello(self):
        data = interpret(Packet(HELLO,1,0,1,bytes([1,6,0,1,0,2])))
        self.assertTrue(data['features']['imu'])
        self.assertFalse(data['features']['charging_requested'])
    def test_c_vectors(self):
        vectors = json.loads((FW / 'build-host/golden-frames.json').read_text())
        self.assertEqual(len(vectors), 97)
        for size, text in enumerate(vectors):
            raw = bytes.fromhex(text)
            decoder = Decoder()
            packets = []
            for i in range(0,len(raw),20):
                packets += decoder.feed(raw[i:i+20])
            self.assertEqual(len(packets), 1)
            p = packets[0]
            self.assertEqual(p.seq, 0xfedcba98)
            self.assertEqual(p.boot, 0x88776655)
            self.assertEqual(p.time_ms, 0x12345678)
            self.assertEqual(p.payload, bytes((j*37)&255 for j in range(size)))
            self.assertEqual(p.encode(), raw)
    def test_crc_vector(self):
        self.assertEqual(binascii.crc_hqx(b'123456789',0xffff), 0x29b1)
    def test_corruption(self):
        valid = Packet(2,1,0,123).encode()
        bad = bytearray(valid); bad[-1] ^= 128
        decoder = Decoder()
        packets = decoder.feed(b'garbage'+bad+valid)
        self.assertEqual(len(packets),1)
        self.assertEqual(packets[0].boot,123)
        self.assertGreater(decoder.rejected,0)
    def test_accel_and_gyro_flags(self):
        values = struct.pack('<6hH',2048,0,0,100,0,0,4)
        d = interpret(Packet(IMU,1,0,1,values))
        self.assertEqual(d['accel_g'],[1,0,0])
        self.assertIsNone(d['gyro_dps'])
        d = interpret(Packet(IMU,1,0,1,values[:-2]+struct.pack('<H',5)))
        self.assertAlmostEqual(d['gyro_dps'][0],100*2000/32768)
    def test_history(self):
        raw = struct.pack('<IHHB',123,1,0,1)+struct.pack('<I6hH',1000,0,0,2048,0,0,0,4)
        d = interpret(Packet(HISTORY,2,0,1,raw))
        self.assertEqual(d['samples'][0]['a_raw'],[0,0,2048])
        self.assertEqual(d['event_seq'],123)
        with self.assertRaises(ValueError):
            interpret(Packet(HISTORY,2,0,1,raw[:-1]))
    def test_status_unknown_voltage(self):
        raw = struct.pack('<H6B5IH',65535,0,0,0,0,2,1,0,0,0,0,0,2)
        d = interpret(Packet(STATUS,3,0,1,raw))
        self.assertIsNone(d['battery_mv'])
        self.assertEqual(d['stream_divisor'],2)
    def test_deduplicated_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = Capture(Path(tmp)/'capture.jsonl')
            p = Packet(1,42,1000,5)
            self.assertTrue(capture.record(p,'USB'))
            self.assertFalse(capture.record(p,'BLE'))
            capture.file.close()
            self.assertEqual(len((Path(tmp)/'capture.jsonl').read_text().splitlines()),1)
    def test_storage_failure_is_not_acknowledged(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = Capture(Path(tmp)/'capture.jsonl')
            p = Packet(EVENT,42,0,5,struct.pack('<BBHHHhH',3,0,0,3700,0,0,0))
            with patch('os.fsync',side_effect=OSError('disk full')):
                with self.assertRaises(CaptureError):
                    capture.record(p,'USB')
            self.assertFalse(capture.seen)
            capture.file.close()

if __name__ == '__main__':
    unittest.main()

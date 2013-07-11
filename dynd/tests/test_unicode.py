import sys
import unittest
from dynd import nd, ndt

if sys.version_info >= (3, 0):
    unicode = str

class TestUnicode(unittest.TestCase):
    def test_array_string(self):
        a = nd.array("Testing 1 2 3")
        self.assertEqual(a.dtype, ndt.string)
        self.assertEqual(str(a), "Testing 1 2 3")
        self.assertEqual(unicode(a), u"Testing 1 2 3")

    def test_bytes_string(self):
        if sys.version_info >= (3, 0):
            a = nd.array(b"Testing 1 2 3")
            b = nd.array([b"First", b"Second"])
        else:
            # In Python 2, str and bytes are the same,
            # so we have to manually request a bytes type
            a = nd.array(b"Testing 1 2 3", type=ndt.bytes)
            b = nd.array([b"First", b"Second"], udtype=ndt.bytes)
        self.assertEqual(a.dtype, ndt.bytes)
        self.assertEqual(b.udtype, ndt.bytes)
        self.assertEqual(nd.as_py(a), b"Testing 1 2 3")
        self.assertEqual(nd.as_py(b), [b"First", b"Second"])

    def test_array_unicode(self):
        a = nd.array(u"\uc548\ub155")
        b = nd.array([u"\uc548\ub155", u"Hello"])
        self.assertEqual(a.dtype, ndt.string)
        self.assertEqual(b.udtype, ndt.string)
        self.assertEqual(unicode(a), u"\uc548\ub155")
        self.assertEqual(nd.as_py(b), [u"\uc548\ub155", u"Hello"])
        # In Python 2, 'str' is not unicode
        if sys.version_info < (3, 0):
            self.assertRaises(UnicodeEncodeError, str, a)

    def test_ascii_decode_error(self):
        a = nd.array(128, type=ndt.uint8).view_scalars("string(1,'A')")
        self.assertRaises(UnicodeDecodeError, a.ucast("string").eval)
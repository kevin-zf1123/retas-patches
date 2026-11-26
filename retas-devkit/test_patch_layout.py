"""Regression tests for declaration validation without a local Stylos installation."""
import struct
import unittest

from patch_layout import LayoutCatalog


def fixture():
    # Only the PE fields used by the address mapper are populated.
    basis = bytearray(0x400)
    pe, opt, table = 0x80, 0x98, 0x178
    basis[:2] = b'MZ'
    struct.pack_into('<I', basis, 60, pe)
    struct.pack_into('<4sHH', basis, pe, b'PE\0\0', 0x14c, 1)
    struct.pack_into('<H', basis, pe + 20, table - opt)
    struct.pack_into('<H', basis, opt, 0x10b)
    struct.pack_into('<II', basis, opt + 32, 4096, 512)
    struct.pack_into('<II', basis, opt + 56, 8192, 512)
    struct.pack_into('<8sIIII', basis, table, b'.data', 32, 4096, 512, 512)
    return basis


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.catalog = LayoutCatalog()
        self.owner = dict(id='example', sites=[dict(rva='0x1008', size=2)], segments=[])
        self.catalog.owners = [(1, self.owner)]
        self.basis = fixture()
        self.linked = self.basis.copy()
        self.linked[520:522] = b'AB'

    def test_declared_site(self):
        self.catalog.add(self.basis, self.linked, 1)
        self.assertEqual(len(self.catalog.outputs), 1)

    def test_overlapping_claim(self):
        self.catalog.owners.append((2, dict(self.owner, id='second')))
        with self.assertRaisesRegex(ValueError, 'conflicting site'):
            self.catalog.add(self.basis, self.linked, 3)

    def test_undeclared_change(self):
        self.linked[523] = 1
        with self.assertRaisesRegex(ValueError, 'Undeclared change'):
            self.catalog.add(self.basis, self.linked, 1)

    def test_unmapped_address(self):
        self.owner['sites'][0]['rva'] = '0x11ff'
        with self.assertRaisesRegex(ValueError, 'Unmapped host region'):
            self.catalog.add(self.basis, self.linked, 1)

    def test_header_ownership(self):
        self.linked[0x98 + 16] = 1
        with self.assertRaisesRegex(ValueError, 'Undeclared PE header'):
            self.catalog.add(self.basis, self.linked, 1)

    def test_independent_plugin_bit(self):
        cases = {4: 'a', 5: 'a', 6: 'b', 7: 'b'}
        self.assertEqual(LayoutCatalog.variants(4, cases),
                         dict(bit=4, mask=2, variants={0: 'a', 2: 'b'}))


if __name__ == '__main__':
    unittest.main()

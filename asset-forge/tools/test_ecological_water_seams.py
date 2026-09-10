import unittest
from audit_ecological_water_seams import compare


class WaterSeams(unittest.TestCase):
    def test_quantization_and_unknown(self):
        r=compare([0,10,20,255,255],[4,15,16,0,255])
        self.assertEqual(r['discontinuities'],1)
        self.assertEqual(r['maximum_known_difference_mm'],10000)
        self.assertEqual(r['ambiguous_pairs'],2)
        self.assertEqual(r['unknown_next_to_near_water'],1)

    def test_unsigned_difference_does_not_wrap(self):
        self.assertEqual(compare([200],[0])['maximum_known_difference_mm'],400000)


if __name__=='__main__':unittest.main()

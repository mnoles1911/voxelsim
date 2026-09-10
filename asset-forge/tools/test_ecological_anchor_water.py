import unittest
from analyze_ecological_anchor_water import summarize,sampled_exposure


class AnchorWaterTests(unittest.TestCase):
    def test_sampled_area_clips_anchors_and_does_not_invent_unsampled_banks(self):
        terrain=[dict(x_mm=x*8000,y_mm=y*8000,active=1,
            distance_water_mm=8000 if x==0 else 90000) for x in range(32) for y in range(32)]
        def anchor(x,d):
            return dict(bank_id=0,x_mm=x,y_mm=0,facts_known=1,active=1,distance_water_mm=d)
        report=sampled_exposure([anchor(0,8000),anchor(8000,90000),
            anchor(16000,32000),anchor(-4001,8000),anchor(252000,8000),
            anchor(0,2147483647)],terrain,['reed'])
        bands={r['band']:r for r in report['species'][0]['bands']}
        self.assertEqual(bands['within_8m']['anchors'],1)
        self.assertEqual(bands['within_8m']['estimated_area_m2'],32*64)
        self.assertAlmostEqual(bands['within_8m']['estimated_anchors_per_hectare'],
            bands['beyond_80m']['estimated_anchors_per_hectare']*31)
        self.assertEqual(bands['8_to_32m']['anchors'],1)
        self.assertIsNone(bands['8_to_32m']['estimated_anchors_per_hectare'])
        self.assertIsNone(bands['unknown_or_saturated']['estimated_anchors_per_hectare'])
        with self.assertRaisesRegex(ValueError,'complete native'):
            sampled_exposure([],terrain[:-1],['reed'])

    def test_exact_facts_preserve_unknowns_and_depth_failures(self):
        def row(distance,water=0,known=1,active=1):
            return dict(bank_id=0,facts_known=known,active=active,water_mm=water,
                distance_water_mm=distance,slope_mm_per_m=0)
        result=summarize([row(8000),row(32000),row(80000),row(80001),
            row(2147483647),row(0,301),row(0,known=0),row(0,active=0)],['reed'])[0]
        counts=result['observations']
        self.assertEqual(result['anchors'],8)
        self.assertEqual(counts['within_8m'],3)
        self.assertEqual(counts['unknown_facts'],1)
        self.assertEqual(counts['unknown_or_saturated'],1)
        self.assertEqual(counts['over_300mm_standing_water'],1)
        self.assertEqual(counts['outside_ecology_scope'],1)

    def test_old_capture_and_invalid_bank_are_refused(self):
        with self.assertRaisesRegex(ValueError,'exact-anchor'):
            summarize([dict(bank_id=0)],['reed'])
        with self.assertRaisesRegex(ValueError,'bank identity'):
            summarize([dict(bank_id=-1,facts_known=1,active=1,water_mm=0,
                distance_water_mm=0,slope_mm_per_m=0)],['reed'])


if __name__=='__main__':unittest.main()

import csv
from pathlib import Path
import tempfile
import unittest
from render_ecological_terrain import render,plot_relief_summary,slope_exposure


class TerrainMapTest(unittest.TestCase):
    def test_slope_rates_use_exact_anchor_facts_and_available_area(self):
        rows=[dict(x_mm=x*8000,y_mm=y*8000,active=1,
            slope_mm_per_m=50 if x==0 else 400) for x in range(32) for y in range(32)]
        def anchor(x,slope,known=1,active=1,tree=1):
            return dict(x_mm=x,y_mm=0,slope_mm_per_m=slope,facts_known=known,
                        active=active,terrain_lattice=tree)
        report=slope_exposure(rows,[anchor(0,50),anchor(0,400),anchor(0,800,tree=0),
            anchor(252000,50),anchor(-4001,50),anchor(0,50,known=0),
            anchor(0,50,active=0)])
        bands={r['band']:r for r in report['bands']}
        self.assertEqual(bands['below_100']['trees']['anchors'],1)
        self.assertEqual(bands['300_to_599']['trees']['anchors'],1)
        self.assertAlmostEqual(bands['below_100']['trees']['estimated_per_hectare'],
            bands['300_to_599']['trees']['estimated_per_hectare']*31)
        self.assertEqual(bands['600_plus']['understory']['anchors'],1)
        self.assertIsNone(bands['600_plus']['understory']['estimated_per_hectare'])
        self.assertEqual(report['unknown_anchor_facts'],1)
        self.assertEqual(report['outside_ecology_anchors'],1)
        with self.assertRaises(ValueError):slope_exposure(rows[:-1],[])
    def test_declined_bounds_are_not_level_building_sites(self):
        result=plot_relief_summary([1000,1000,-9223372036854775808],[1400,1700,9223372036854775807])
        self.assertEqual(result['bounded_fraction'],2/3)
        self.assertEqual(result['within_500mm_relief_bound_fraction'],1/3)
        self.assertEqual(result['median_relief_bound_mm'],550)
    def test_exports_keep_unknown_water_and_runtime_tree_classification(self):
        # Deliberately synthetic unit-test data, never displayed as world output.
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            rows=[]
            for y in range(32):
                for x in range(32):
                    rows.append(dict(x_mm=x*8000,y_mm=y*8000,surface_mm=x*1000,
                        slope_mm_per_m=125,curvature=-1,heat=-1,talus=-1,water_mm=0,
                        biome=3,active=1,stand=0,height_target=650,tree_keep=750,
                        feature_strength=0,distance_water_mm=2147483647,twi_milli=-2147483648))
            with (root/'terrain-samples.csv').open('w',newline='') as stream:
                writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
            with (root/'terrain-placement.csv').open('w',newline='') as stream:
                stream.write('bank_id,seed_slot,x_mm,y_mm,z_mm,layer,yaw,terrain_lattice\n'
                    '0,0,0,0,0,2,0,1\n1,0,0,0,0,3,0,0\n0,0,300000,0,0,0,0,1\n')
            result=render(root)
            self.assertEqual(result['tree_anchors'],1)
            self.assertEqual(result['understory_anchors'],1)
            self.assertEqual(result['elevation_range_m'],[0,31])
            self.assertEqual(result['water_distance_unknown_fraction'],1)
            self.assertFalse(result['slope_exposure']['available'])
            self.assertTrue((root/'terrain-placement-map.png').is_file())
            # Duplicate/missing samples must not quietly paint an invented map.
            with (root/'terrain-samples.csv').open('a') as stream:stream.write(','.join(str(v) for v in rows[0].values())+'\n')
            with self.assertRaises(ValueError):render(root)


if __name__=='__main__':unittest.main()

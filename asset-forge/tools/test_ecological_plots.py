import unittest
import numpy as np
from analyze_ecological_plots import box_hits,rotated_grid,runtime_variant,footprint_dry


class PlotTest(unittest.TestCase):
    def test_water_check_requires_every_column_to_be_known_and_dry(self):
        self.assertTrue(footprint_dry({'plot_tested_cells':2500,'plot_water_max_mm':0,'plot_water_channels_verified':1}))
        self.assertFalse(footprint_dry({'plot_tested_cells':2499,'plot_water_max_mm':0,'plot_water_channels_verified':1}))
        self.assertFalse(footprint_dry({'plot_tested_cells':2500,'plot_water_max_mm':1,'plot_water_channels_verified':1}))
        # Old exports sampled all columns but silently omitted inland hydrology.
        self.assertFalse(footprint_dry({'plot_tested_cells':2500,'plot_water_max_mm':0}))
        self.assertFalse(footprint_dry({'plot_tested_cells':2500,'plot_water_max_mm':0,'plot_water_channels_verified':0}))
        self.assertFalse(footprint_dry({'water_mm':0}))

    def test_legacy_draw_wraps_to_actual_bank_file(self):
        self.assertEqual(runtime_variant(['a','b','c'],3),'a')
        self.assertEqual(runtime_variant(['a','b','c'],2),'c')
        with self.assertRaises(ValueError):runtime_variant([],0)
        with self.assertRaises(ValueError):runtime_variant(['a'],-1)

    def test_half_open_volume_and_height_do_not_count_branches_above_roof(self):
        data=np.zeros((2,3,5),dtype=np.uint8);data[1,2,4]=16
        origin=np.array([-3,-4,10])
        self.assertFalse(box_hits(data,origin,[-3,-4,10],[0,0,14]))
        self.assertTrue(box_hits(data,origin,[-3,-4,10],[0,0,15]))
        self.assertFalse(box_hits(data,origin,[0,0,0],[5,5,20]))

    def test_rotated_source_uses_exact_negative_coordinate_convention(self):
        data=np.zeros((2,3,4),dtype=np.uint8);data[0,0,2]=16
        expected=[(-1,-2,1),(2,-1,1),(1,2,1),(-2,1,1)]
        for yaw,cell in enumerate(expected):
            rotated,origin=rotated_grid(data,(-1,-2,-1),yaw)
            self.assertTrue(box_hits(rotated,origin,cell,np.array(cell)+1))


if __name__=='__main__':unittest.main()

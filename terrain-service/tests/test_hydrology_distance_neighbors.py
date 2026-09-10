import unittest
import numpy as np
from scipy.ndimage import distance_transform_edt
from terrain_service.bake.hydrology_distance import distance_from_final_masks


class FinalMaskDistance(unittest.TestCase):
    def test_neighbor_lake_matches_one_world_transform(self):
        # A lake lives entirely east of the center tile. Its bank influence
        # must cross the seam even though the center contains no water.
        edge=32;cell=20.
        world=np.zeros((edge*3,edge*3),bool)
        world[40:53,65:71]=True
        masks={(x-1,y-1):world[y*edge:(y+1)*edge,x*edge:(x+1)*edge]
               for y in range(3) for x in range(3)}
        result=distance_from_final_masks(masks,(0,0),cell_m=cell)
        exact=distance_transform_edt(~world)[32:64,32:64]*cell
        expected=np.minimum(np.floor(exact.reshape(8,4,8,4).min(axis=(1,3))/2),255).astype(np.uint8)
        np.testing.assert_array_equal(result,expected)
        self.assertLess(result[3,-1],255)
        # Mapping insertion order must not change a result.
        np.testing.assert_array_equal(result,distance_from_final_masks(dict(reversed(list(masks.items()))),(0,0),cell_m=cell))

    def test_missing_neighbor_cannot_be_assumed_dry(self):
        with self.assertRaisesRegex(ValueError,'Complete neighbor'):
            distance_from_final_masks({(0,0):np.zeros((32,32),bool)},(0,0),cell_m=20)

    def test_all_dry_and_insufficient_halo(self):
        masks={(x,y):np.zeros((32,32),bool) for x in (-1,0,1) for y in (-1,0,1)}
        self.assertTrue((distance_from_final_masks(masks,(0,0),cell_m=20)==255).all())
        with self.assertRaisesRegex(ValueError,'distance range'):
            distance_from_final_masks(masks,(0,0),cell_m=1)


if __name__=='__main__':unittest.main()

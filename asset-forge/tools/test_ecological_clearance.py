import unittest
import numpy as np
from analyze_ecological_clearance import rotated_projection,stamp


class GeometryTest(unittest.TestCase):
    def test_rotations_match_integer_source_voxels_and_height_band(self):
        data=np.zeros((2,3,4),dtype=np.uint8)
        data[0,0,0]=1 # below the ground plane
        data[1,2,1]=1
        data[0,0,2]=1
        for yaw in range(4):
            mask,(ox,oy)=rotated_projection(data,(-1,-2,-1),yaw,2)
            actual={(int(x)+ox,int(y)+oy) for x,y in zip(*np.nonzero(mask))}
            second=((-1,-2),(2,-1),(1,2),(-2,1))[yaw]
            self.assertEqual(actual,{(0,0),second})
            low,origin=rotated_projection(data,(-1,-2,-1),yaw,1)
            self.assertEqual({(int(x)+origin[0],int(y)+origin[1]) for x,y in zip(*np.nonzero(low))},{(0,0)})

    def test_stamp_clips_negative_edges_without_wrapping(self):
        dest=np.zeros((5,5),dtype=bool)
        stamp(dest,np.ones((3,2),dtype=bool),-1,1)
        self.assertEqual(int(dest.sum()),4)
        self.assertTrue(dest[:2,1:3].all())
        self.assertFalse(dest[-1,:].any())


if __name__=='__main__':unittest.main()

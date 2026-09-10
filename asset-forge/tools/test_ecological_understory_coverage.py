import unittest
import numpy as np
from analyze_ecological_understory_coverage import projection,stamp


class ProjectionTests(unittest.TestCase):
    def test_full_height_projection_and_rotated_negative_origin(self):
        data=np.zeros((2,3,4),dtype=np.uint8)
        data[0,0,3]=1
        data[1,2,0]=1
        expected=[{(-1,-2),(0,0)},{(2,-1),(0,0)},{(1,2),(0,0)},{(-2,1),(0,0)}]
        for yaw in range(4):
            mask,(ox,oy)=projection(data,(-1,-2,-1),yaw)
            actual={(int(x)+ox,int(y)+oy) for x,y in zip(*np.nonzero(mask))}
            self.assertEqual(actual,expected[yaw])
            union=np.zeros((10,10),dtype=bool)
            stamp(union,mask,ox+4,oy+4)
            stamp(union,mask,ox+4,oy+4)
            self.assertEqual(int(union.sum()),2) # overlapping instances are not double-counted


if __name__=='__main__':unittest.main()

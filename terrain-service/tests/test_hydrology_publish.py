import hashlib
from pathlib import Path
import tempfile
import unittest
import numpy as np
from terrain_service import tile_codec as tc
from terrain_service.bake.hydrology_publish import stage
from terrain_service.bake.hydrology_sources import write_bake_water_source


class HydrologyPublication(unittest.TestCase):
    def fixture(self,root):
        tiles=root/'tiles';masks=root/'masks';tiles.mkdir();masks.mkdir()
        edge=512
        for x in (-1,0,1):
            for y in (-1,0,1):
                plane=np.zeros((128,128),np.uint8)
                tile=tc.TileV2(seed=42,x=x,y=y,size=edge,
                    elevation_cp=np.full((edge,edge),100,np.int16),bake_ver=28,
                    place_dist_water=np.full_like(plane,255),place_twi=plane+45,
                    place_talus=plane+6,place_curv=plane+120,place_heat=plane+90)
                encoded=tc.encode_v2(tile);(tiles/f'{x}_{y}.vxtl').write_bytes(encoded)
                wet=np.zeros((edge,edge),bool)
                if (x,y)==(1,0):wet[240:270,1:6]=True
                write_bake_water_source(masks/f'{x}_{y}.water-source.npz',
                    np.packbits(wet,axis=1,bitorder='little'),encoded,provider_id='fixture',cell_m=1.875)
        return tiles,masks

    def test_stages_neighbor_influence_without_source_mutation(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);tiles,masks=self.fixture(root)
            original=(tiles/'0_0.vxtl').read_bytes()
            result=stage(tiles,masks,root/'out',[(0,0)])
            updated=tc.decode_v2((root/'out/0_0.vxtl').read_bytes())
            before=tc.decode_v2(original)
            self.assertLess(int(updated.place_dist_water[63,-1]),5)
            self.assertEqual(int(updated.place_dist_water[63,0]),255)
            for name in ('elevation_cp','place_twi','place_talus','place_curv','place_heat'):
                np.testing.assert_array_equal(getattr(before,name),getattr(updated,name))
            self.assertEqual(original,(tiles/'0_0.vxtl').read_bytes())
            self.assertFalse(result['activated'])
            self.assertEqual(result['outputs'][0]['sha256'],hashlib.sha256((root/'out/0_0.vxtl').read_bytes()).hexdigest())
            second=stage(tiles,masks,root/'repeat',[(0,0)])
            self.assertEqual(result,second)

    def test_missing_dependency_never_creates_publication(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);tiles,masks=self.fixture(root)
            (masks/'1_1.water-source.npz').unlink()
            with self.assertRaises(FileNotFoundError):stage(tiles,masks,root/'out',[(0,0)])
            self.assertFalse((root/'out').exists())


if __name__=='__main__':unittest.main()

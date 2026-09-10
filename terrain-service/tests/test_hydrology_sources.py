import hashlib
from pathlib import Path
import tempfile
import unittest
import numpy as np
from terrain_service import tile_codec as tc
from terrain_service.bake.hydrology_sources import read_published_freshwater_mask, write_bake_water_source, read_bake_water_source
from terrain_service.bake.placement import pack_final_water_mask


class PublishedFreshwater(unittest.TestCase):
    def test_complete_bake_mask_roundtrip_and_hash_refusal(self):
        z=np.ones((16,16),np.float32);z[0,0]=-1
        lake=np.zeros_like(z,dtype=bool);lake[1,2]=True
        river=np.zeros_like(lake);river[3,4]=True
        packed=pack_final_water_mask(z,lake,river)
        tile=tc.TileV2(seed=42,x=-3,y=2,size=16,block_log2=4,
            elevation_cp=np.ones((16,16),np.int16))
        encoded=tc.encode_v2(tile)
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'source.npz'
            write_bake_water_source(path,packed,encoded,provider_id='test',cell_m=1.875)
            mask,info=read_bake_water_source(path,encoded)
            np.testing.assert_array_equal(mask,(z<=0)|lake|river)
            self.assertTrue(info['includes_sea'])
            with self.assertRaisesRegex(ValueError,'does not match'):
                read_bake_water_source(path,encoded+b'changed')

    def test_depth_union_and_source_identity(self):
        size=16;dry=np.full((size,size),-1,np.int16)
        river=dry.copy();river[2,3]=0;river[2,4]=17;river[2,5]=-200
        lake=dry.copy();lake[10,11]=4
        tile=tc.TileV2(seed=42,x=-3,y=2,size=size,block_log2=4,
            elevation_cp=np.full((size,size),-100,np.int16),water_cp=river,
            bathy_depth=lake,bathy_shore=dry.copy())
        data=tc.encode_v2(tile)
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'tile.vxtl';path.write_bytes(data)
            mask,info=read_published_freshwater_mask(path)
        self.assertEqual(int(mask.sum()),3)
        self.assertFalse(mask[2,5]) # negative level-band codes are not standing water
        self.assertFalse(info['includes_sea']) # negative elevation CPs are not sea samples
        self.assertEqual(info['sha256'],hashlib.sha256(data).hexdigest())
        self.assertEqual(info['tile'],[-3,2])

    def test_missing_survey_is_not_dry(self):
        tile=tc.TileV2(seed=42,x=0,y=0,size=16,block_log2=4,
            elevation_cp=np.zeros((16,16),np.int16))
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'tile.vxtl';path.write_bytes(tc.encode_v2(tile))
            with self.assertRaisesRegex(ValueError,'depth planes required'):
                read_published_freshwater_mask(path)


if __name__=='__main__':unittest.main()

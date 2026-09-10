import copy,csv,tempfile,unittest
from pathlib import Path
from analyze_ecological_walk import read_frames
from compare_size_culling_ecological_walks import validate_inputs,exercise,FLAG
class SizeComparisonTests(unittest.TestCase):
    def pair(self):
        a={'arguments':['-VoxelDetailRingMeters=256','-ResX=1280','-ResY=720','-UserDir=a','-abslog=a'],
           'configurationSha256':'a'*64,'speciesManifestSha256':'b'*64,'detailCacheManifestSha256':'c'*64,'runtimeModuleHashes':{'Voxel.dll':'d'*64}}
        b=copy.deepcopy(a);b['arguments']=[v for v in b['arguments'] if not v.startswith(('-UserDir=','-abslog='))]+[FLAG,'-UserDir=b','-abslog=b']
        return a,b
    def test_exact_inputs_and_full_radius(self):
        a,b=self.pair();validate_inputs(a,b)
        for field in ('configurationSha256','speciesManifestSha256','detailCacheManifestSha256'):
            bad=copy.deepcopy(b);bad[field]='e'*64
            with self.assertRaises(ValueError):validate_inputs(a,bad)
        bad=copy.deepcopy(b);bad['runtimeModuleHashes']['Voxel.dll']='e'*64
        with self.assertRaises(ValueError):validate_inputs(a,bad)
        for extra in ('-ResX=640','-VoxelSpawnAt=1,2','-VoxelDetailShadows'):
            bad=copy.deepcopy(b);bad['arguments'].append(extra)
            with self.assertRaises(ValueError):validate_inputs(a,bad)
        for r in (a,b):r['arguments']=[v.replace('Meters=256','Meters=48') for v in r['arguments']]
        with self.assertRaises(ValueError):validate_inputs(a,b)
    def test_actual_policy_exercise(self):
        line='DetailSizeCull key=123 bounds=xxx scale=1 startM=27.20 endM=32.00 ringM=256.00 fallback=0'
        self.assertEqual(exercise(line,True)[0]['end_m'],32)
        for text,on in ((line,False),('',True),(line.replace('endM=32.00','endM=300.00'),True),(line.replace('endM=32.00','endM=256.00'),True)):
            with self.assertRaises(ValueError):exercise(text,on)
    def test_walk_expansion_duplicates_and_corruption(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'frames.csv'
            initial=['FrameTime','GPUTime'];full=initial+['VoxelStream/LateMs','VoxelStream/AppearancePackMs','VoxelStream/AppearancePackMs']
            with path.open('w',newline='') as f:
                w=csv.writer(f);w.writerow(initial);w.writerow([10,5]);w.writerow([20,7,4,111,222]);w.writerow(full);w.writerow(['[HasHeaderRowAtEnd]','1'])
            rows,elapsed,diag=read_frames(path)
            self.assertAlmostEqual(elapsed,.03);self.assertEqual(rows[0][1]['VoxelStream/LateMs'],'0');self.assertEqual(rows[1][1]['VoxelStream/LateMs'],'4')
            self.assertNotIn('VoxelStream/AppearancePackMs',rows[1][1]);self.assertIn('VoxelStream/AppearancePackMs',diag['ambiguous_columns_excluded'])
            for bad in ('bad','nan','-1','0'):
                path.write_text('FrameTime,GPUTime\n'+bad+',5\n')
                with self.assertRaises(ValueError):read_frames(path)
            path.write_text('FrameTime,GPUTime,FrameTime\n10,5,20\n')
            with self.assertRaises(ValueError):read_frames(path)
if __name__=='__main__':unittest.main()

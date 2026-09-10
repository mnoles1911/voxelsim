import csv
import tempfile
import unittest
from pathlib import Path
from analyze_ecological_fields import analyze


class SurveyTest(unittest.TestCase):
    def test_area_boundary_components_and_transect_entries(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            with (root/'field-samples.csv').open('w',newline='') as stream:
                writer=csv.writer(stream)
                writer.writerow(('world_seed','x_mm','y_mm','community','stand','target_height_per_mille'))
                for y in range(256):
                    for x in range(256):
                        stand=3 if x in (16,17) and y in (16,17) else 4 if x<2 and y<2 else 0
                        writer.writerow((42,x*8000,y*8000,0,stand,650))
            seed=analyze(root)['seeds'][0]
            self.assertEqual(seed['stand_area_fractions']['ancient'],4/65536)
            ancient=seed['features']['ancient'];thicket=seed['features']['thicket']
            self.assertEqual(ancient['complete_patches'],1)
            self.assertEqual(ancient['median_sampled_patch_area_m2'],256)
            self.assertEqual(ancient['transect_entries'],2)
            self.assertEqual(thicket['patches_intersecting_survey'],1)
            self.assertEqual(thicket['complete_patches'],0)
            self.assertEqual(thicket['transect_entries'],0)


if __name__=='__main__':unittest.main()

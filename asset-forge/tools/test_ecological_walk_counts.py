import unittest
from analyze_ecological_walk import collision_counts


class CollisionCountTests(unittest.TestCase):
    def test_counts_are_summed_and_absent_instrumentation_is_not_zero(self):
        rows=[{'VoxelStream/CollisionSweepCalls':'6','VoxelStream/CollisionPreparations':'1'},
              {'VoxelStream/CollisionSweepCalls':'2','VoxelStream/CollisionPreparations':'1'}]
        result=collision_counts(rows)
        self.assertEqual(result['totals']['CollisionSweepCalls'],8)
        self.assertEqual(result['sweeps_per_preparation'],4)
        self.assertIsNone(collision_counts([{'VoxelStream/CollisionSweepCalls':'8'}])['sweeps_per_preparation'])
        self.assertEqual(collision_counts([{}])['totals'],{})
        self.assertIsNone(collision_counts([{'VoxelStream/CollisionSweepCalls':'0',
            'VoxelStream/CollisionPreparations':'0'}])['sweeps_per_preparation'])

    def test_invalid_counts_fail(self):
        for value in ('nan','-1','0.5'):
            with self.assertRaisesRegex(ValueError,'Invalid collision count'):
                collision_counts([{'VoxelStream/CollisionSweepCalls':value}])


if __name__=='__main__':unittest.main()

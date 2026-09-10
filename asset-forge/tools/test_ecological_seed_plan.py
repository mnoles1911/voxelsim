import unittest
from prepare_ecological_preview import seed_plan


class SeedPlanTest(unittest.TestCase):
    def test_species_can_have_different_seed_sets_without_changing_defaults(self):
        defaults=[7,12,14]
        result=seed_plan(['oak','birch'],defaults,{'birch':[7,12,26,31]})
        self.assertEqual(result,{'oak':[7,12,14],'birch':[7,12,26,31]})
        result['oak'].append(35)
        self.assertEqual(defaults,[7,12,14])

    def test_invalid_or_ambiguous_plans_refuse(self):
        for overrides in ({'unknown':[7]},{'oak':[]},{'oak':[7,7]},{'oak':[True]},{'oak':[-1]},{'oak':['7']},{'oak':[[7]]}):
            with self.assertRaises(ValueError):seed_plan(['oak'],[7,12],overrides)
        with self.assertRaises(ValueError):seed_plan(['oak','oak'],[7])


if __name__=='__main__':unittest.main()

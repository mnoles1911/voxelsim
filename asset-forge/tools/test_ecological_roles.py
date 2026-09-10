import unittest
from audit_ecological_roles import audit


class CoverageTests(unittest.TestCase):
    def test_zero_weight_and_empty_profiles_cannot_fill_inventory_gaps(self):
        def profile(name,weights,variants,kind='tree',role=None):
            return dict(species=name,community_weights_per_mille=weights,variants=variants,kind=kind,cover_role=role)
        variant=dict(id='oak-7',size_class='large',growth_form='open',height_mm=18000)
        config=dict(preview_only=True,communities=[dict(id='dry'),dict(id='wet')],profiles=[
            profile('oak',[1000,0],[variant]),profile('empty',[1000,1000],[]),
            profile('reed',[0,1000],[dict(id='reed-1')],'reed','wetland')])
        result=audit(config)
        dry,wet=result['communities']
        self.assertTrue(result['preview_only'])
        self.assertEqual(dry['tree_species'],['oak'])
        self.assertEqual(dry['missing_tree_sizes'],['small','medium'])
        self.assertEqual(dry['tallest_available_tree_mm'],18000)
        self.assertEqual(wet['tree_species'],[])
        self.assertIsNone(wet['tallest_available_tree_mm'])
        self.assertEqual(wet['cover_species_by_role']['wetland'],['reed'])
        self.assertIn('wetland',dry['absent_cover_roles'])


if __name__=='__main__':unittest.main()

import copy
import unittest
from prepare_ecological_preview import apply_low_cover_pilot


class LowCoverTests(unittest.TestCase):
    def test_only_nonfavored_dry_understory_weights_change(self):
        def p(name,kind,height,role):
            return dict(species=name,kind=kind,cover_role=role,variants=[dict(height_mm=height)],community_weights_per_mille=[0,1,500,1000])
        profiles=[p('tree','tree',20000,None),p('reed','reed',2000,'wetland'),
            p('short','grass',1000,'sun'),p('tall','grass',1001,'sun'),p('flower','flower',200,'spring-woodland')]
        before=copy.deepcopy(profiles)
        self.assertEqual(apply_low_cover_pilot(dict(profiles=profiles)),['short'])
        self.assertEqual(profiles[:3],before[:3])
        for profile in profiles[3:]:self.assertEqual(profile['community_weights_per_mille'],[0,1,125,250])
        self.assertEqual(profiles[3]['variants'],before[3]['variants'])


if __name__=='__main__':unittest.main()

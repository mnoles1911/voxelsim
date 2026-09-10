import unittest
from analyze_ecological_world import capture_event_times


class ClockTests(unittest.TestCase):
    def test_explicit_clock_does_not_use_cached_log_prefix(self):
        log='\n'.join('[2026.09.09-18.00.00:000] EcologyWorld '+name+' mono='+str(value)
            for name,value in [('MEASURE_BEGIN',100),('OVERVIEW',120),('MEASURE_END',140)])
        self.assertEqual(capture_event_times(log),(20,40,'monotonic'))
        with self.assertRaisesRegex(ValueError,'Incomplete/duplicate'):
            capture_event_times(log.replace(' mono=120',''))
        with self.assertRaisesRegex(ValueError,'order'):
            capture_event_times(log.replace('mono=140','mono=99'))

    def test_old_logs_remain_explicitly_legacy(self):
        log='\n'.join('[2026.09.09-18.00.'+second+':000] EcologyWorld '+name
            for name,second in [('MEASURE_BEGIN','00'),('OVERVIEW','20'),('MEASURE_END','40')])
        self.assertEqual(capture_event_times(log),(20,40,'legacy_log_clock'))


if __name__=='__main__':unittest.main()

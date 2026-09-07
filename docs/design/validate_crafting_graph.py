"""Check logical bootstrap routes. This does not simulate actual crafting costs."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
graph = json.loads((HERE / 'crafting-capabilities-v1.json').read_text(encoding='utf-8'))
resources = {n['id'] for n in graph['resources']}
capabilities = {n['id'] for n in graph['capabilities']}
nodes = resources | capabilities
assert len(nodes) == len(graph['resources']) + len(graph['capabilities']), 'Duplicate node ID'
assert len({r['id'] for r in graph['rules']}) == len(graph['rules']), 'Duplicate rule ID'
for rule in graph['rules']:
    assert rule['grants'] and set(rule['grants']) <= capabilities, rule['id']
    assert all(rule['requires_any_groups']), f'Empty OR group: {rule["id"]}'
    referenced = set(rule['requires_all']) | {x for g in rule['requires_any_groups'] for x in g}
    assert referenced <= nodes, f'Unknown requirement: {rule["id"]}'

def reachable(excluded_resources=(), excluded_rules=()):
    available = resources - set(excluded_resources)
    while True:
        gained = set()
        for rule in graph['rules']:
            if rule['id'] in excluded_rules:
                continue
            if (set(rule['requires_all']) <= available
                    and all(available.intersection(g) for g in rule['requires_any_groups'])):
                gained.update(rule['grants'])
        if gained <= available:
            return available
        available |= gained

checks = []
def check(name, available, must=(), absent=()):
    assert set(must) <= available, f'{name}: unreachable {set(must)-available}'
    assert not set(absent) & available, f'{name}: unintended unlock {set(absent)&available}'
    checks.append(dict(name=name, result='PASS', reachable_capabilities=len(available & capabilities)))

check('All defined capabilities have a bootstrap route', reachable(), capabilities)
check('No metals: raft, pottery, textiles and farming remain possible',
      reachable(['native_copper','tin_ore','surface_iron','deep_iron']),
      ['log_raft','bamboo_raft','pottery','textiles','farming'],
      ['copper_cast','bronze_cast','iron_bloom','steel_control'])
check('No tin: iron, steel and water power remain possible', reachable(['tin_ore']),
      ['iron_tools','steel_control','water_power'], ['bronze_cast'])
check('No copper: surface iron avoids a copper-pick bootstrap cycle', reachable(['native_copper']),
      ['iron_tools','steel_control'], ['copper_cast','bronze_cast'])
check('No power site: advanced hand workshop remains possible', reachable(['water_site','wind_site']),
      ['steel_control','precision_tools','hand_pump','plank_boat'],
      ['water_power','wind_power','powered_mill','powered_pump','helve_hammer'])
check('No bamboo: log raft remains possible', reachable(['bamboo']), ['log_raft'], ['bamboo_raft'])
check('No iron: bronze-driven milling remains possible', reachable(['surface_iron','deep_iron']),
      ['bronze_tools','water_power','powered_mill'], ['iron_tools','steel_control'])
check('No bellows: thermal metallurgy stays blocked', reachable(excluded_rules=['sew_bellows']),
      ['log_raft','pottery'], ['copper_cast','iron_bloom','steel_control'])
check('Ore alone cannot bypass bloom consolidation', reachable(excluded_rules=['hand_consolidate']),
      ['iron_bloom'], ['iron_billet','iron_tools','steel_control'])
report = dict(scope='Logical reachability only; not gameplay, mass, quantity, geography or economy validation.',
              resources=len(resources), capabilities=len(capabilities), rules=len(graph['rules']), checks=checks)
(HERE / 'crafting-graph-validation.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print(json.dumps(report, indent=2))

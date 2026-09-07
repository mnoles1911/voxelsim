import json
from pathlib import Path

root = Path('D:/voxelsim')
out = root / 'docs/design'
out.mkdir(exist_ok=True)
resources = {
    'stone_flaking': 'Accessible knappable stone', 'stone_tough': 'Accessible tough stone',
    'wood': 'Gatherable branches and standing timber', 'fiber': 'Accessible plant fiber',
    'tinder': 'Gatherable tinder', 'clay': 'Accessible pottery clay',
    'refractory': 'Suitable accessible refractory material', 'hide': 'Obtainable raw hide',
    'bone': 'Obtainable bone or antler', 'bamboo': 'Accessible bamboo stand',
    'native_copper': 'Accessible native copper pieces', 'tin_ore': 'Tin deposit',
    'surface_iron': 'Accessible iron-bearing material suitable for the designed process',
    'deep_iron': 'Hard-rock iron deposit', 'abrasive': 'Suitable abrasive stone',
    'grain': 'Harvestable grain or seeds', 'water_site': 'Usable flowing-water site',
    'wind_site': 'Usable exposed wind site', 'water': 'Accessible water',
}
caps = {
    'knap': ('shaping', 'Shape flaked stone'), 'cut': ('cutting', 'Cut and scrape soft material'),
    'pound': ('shaping', 'Pound and crush'), 'cordage': ('textiles', 'Make cordage and lashings'),
    'dig': ('extraction', 'Dig soil and soft deposits'), 'fire': ('thermal', 'Maintain a controlled fire'),
    'chop': ('woodworking', 'Chop timber with a hafted tool'),
    'adze': ('woodworking', 'Shape wood with an adze'),
    'pierce': ('textiles', 'Pierce and sew'), 'basketry': ('storage', 'Make woven containers'),
    'paddle': ('transport', 'Shape a paddle'), 'bamboo_raft': ('transport', 'Assemble a bamboo raft'),
    'log_raft': ('transport', 'Assemble a log raft'), 'rough_stock': ('woodworking', 'Split and dress rough stock'),
    'pottery': ('thermal', 'Form and fire basic pottery'),
    'food_storage': ('storage', 'Use ceramic food storage'),
    'charcoal': ('thermal', 'Produce charcoal'), 'leather': ('textiles', 'Prepare leather'),
    'bellows': ('thermal', 'Supply controlled air with bellows'),
    'casting_ceramics': ('thermal', 'Make suitable crucibles and molds'),
    'copper_cast': ('metallurgy', 'Cast copper'), 'copper_tools': ('metallurgy', 'Assemble copper tools'),
    'mining': ('extraction', 'Work hard mineral deposits'), 'tin_supply': ('metallurgy', 'Prepare tin supply'),
    'bronze_cast': ('metallurgy', 'Produce and cast tin bronze'),
    'bronze_tools': ('metallurgy', 'Assemble bronze tools'),
    'working_surface': ('shaping', 'Prepare a basic anvil surface'),
    'forge': ('thermal', 'Reheat metal for working'),
    'iron_bloom': ('metallurgy', 'Produce an iron bloom'), 'iron_billet': ('metallurgy', 'Consolidate bloom manually'),
    'iron_tools': ('metallurgy', 'Forge and assemble iron tools'),
    'saw_chisel': ('woodworking', 'Make serviceable saw and chisel tools'),
    'joinery': ('woodworking', 'Make fitted wooden assemblies'),
    'barrel': ('storage', 'Make a coopered liquid container'), 'cart': ('transport', 'Make a cargo cart'),
    'textiles': ('textiles', 'Spin and weave cloth'), 'sail_rig': ('transport', 'Build a working sail rig'),
    'fittings': ('metallurgy', 'Make suitable metal fittings'),
    'hand_mill': ('food', 'Grind grain manually'), 'farming': ('food', 'Cultivate and harvest crops'),
    'power_train': ('power', 'Build shafts, bearings, and gearing'),
    'water_power': ('power', 'Capture water power'), 'wind_power': ('power', 'Capture wind power'),
    'powered_mill': ('power', 'Drive a grain mill'), 'helve_hammer': ('power', 'Mechanize hammer work'),
    'steel_control': ('metallurgy', 'Make and treat steel repeatably by hand'),
    'precision_tools': ('woodworking', 'Make files and augers'),
    'hand_pump': ('power', 'Build a manually driven drainage pump'),
    'powered_pump': ('power', 'Power the drainage pump'),
    'plank_boat': ('transport', 'Build an advanced plank boat'),
}
rules = []
def rule(id, grants, all=(), any=(), note=''):
    rules.append(dict(id=id, grants=[grants], requires_all=list(all),
                      requires_any_groups=[list(g) for g in any], note=note))
rule('shape_flakes', 'knap', ['stone_flaking','stone_tough'])
rule('flake_edge', 'cut', ['knap'])
rule('hammerstone', 'pound', ['stone_tough'])
rule('twist_fiber', 'cordage', ['fiber','cut'])
rule('digging_stick', 'dig', ['wood','cut'])
rule('friction_fire', 'fire', ['wood','tinder','cordage','cut'])
rule('ground_stone_axe', 'chop', ['stone_tough','abrasive','wood','cordage','pound'])
rule('stone_adze', 'adze', ['stone_tough','abrasive','wood','cordage','pound'])
rule('bone_awl', 'pierce', ['bone','abrasive','cut'])
rule('weave_basket', 'basketry', ['fiber','cut'])
rule('shape_paddle', 'paddle', ['wood','adze'])
rule('lash_bamboo_raft', 'bamboo_raft', ['bamboo','chop','cordage','paddle'])
rule('lash_log_raft', 'log_raft', ['wood','chop','cordage','paddle'])
rule('split_and_dress', 'rough_stock', ['wood','chop','adze','pound'])
rule('form_fire_pots', 'pottery', ['clay','water','dig','fire'])
rule('lidded_crock', 'food_storage', ['pottery'])
rule('charcoal_burn', 'charcoal', ['wood','chop','fire'])
rule('prepare_hide', 'leather', ['hide','cut','water'], note='Preparation agents and quantities belong in detailed recipes.')
rule('sew_bellows', 'bellows', ['leather','pierce','rough_stock','cordage'])
rule('fire_casting_equipment', 'casting_ceramics', ['pottery','refractory'])
rule('native_copper_cast', 'copper_cast', ['native_copper','casting_ceramics','charcoal','bellows'], note='Native copper bootstrap; ore smelting is a separate future recipe.')
rule('haft_copper_heads', 'copper_tools', ['copper_cast','wood','cordage'])
rule('metal_pick', 'mining', any=[['copper_tools','bronze_tools','iron_tools']])
rule('prepare_tin', 'tin_supply', ['tin_ore','mining','casting_ceramics','charcoal','bellows'])
rule('alloy_bronze', 'bronze_cast', ['tin_supply','copper_cast'])
rule('haft_bronze_heads', 'bronze_tools', ['bronze_cast','wood','cordage'])
rule('stone_anvil', 'working_surface', ['stone_tough','pound'], note='Bootstrap working surface; limited throughput and wear compared with later anvils.')
rule('build_forge', 'forge', ['clay','charcoal','bellows'])
rule('surface_iron_bloom', 'iron_bloom', ['surface_iron','refractory','pottery','charcoal','bellows','pound'], note='Resource-specific surface route; deliberately does not require bronze or a metal pick.')
rule('mined_iron_bloom', 'iron_bloom', ['deep_iron','mining','refractory','pottery','charcoal','bellows','pound'])
rule('hand_consolidate', 'iron_billet', ['iron_bloom','working_surface','forge','pound'], note='Detailed process must model tongs/handling; these can bootstrap with suitable wooden handling tools.')
rule('forge_iron_tools', 'iron_tools', ['iron_billet','working_surface','forge','wood','cordage'])
rule('make_joinery_tools', 'saw_chisel', ['abrasive'], [['bronze_tools','iron_tools']])
rule('fit_wood', 'joinery', ['saw_chisel','rough_stock'])
rule('cooper_barrel', 'barrel', ['joinery','cordage'], note='Wooden hoops provide an early option; metal hoops are an upgrade.')
rule('build_cart', 'cart', ['joinery','fittings'])
rule('spin_weave', 'textiles', ['fiber','rough_stock','cordage'])
rule('rig_sail', 'sail_rig', ['textiles','joinery','cordage'], [['bamboo_raft','log_raft','plank_boat']])
rule('cast_fittings', 'fittings', ['bronze_cast'])
rule('forge_fittings', 'fittings', ['iron_tools'])
rule('grind_grain', 'hand_mill', ['stone_tough','pound','grain'])
rule('cultivate', 'farming', ['dig','grain','water'])
rule('fit_transmission', 'power_train', ['joinery','fittings'])
rule('waterwheel', 'water_power', ['power_train','water_site'])
rule('windmill', 'wind_power', ['power_train','wind_site','textiles'])
rule('drive_mill', 'powered_mill', ['hand_mill'], [['water_power','wind_power']])
rule('drive_hammer', 'helve_hammer', ['iron_tools','forge'], [['water_power','wind_power']])
rule('manual_controlled_steel', 'steel_control', ['iron_billet','iron_tools','refractory','charcoal','forge','abrasive'], note='Reliable carburizing/refining and heat-treatment process; powered hammer optional.')
rule('make_files_augers', 'precision_tools', ['steel_control','joinery'])
rule('piston_pump', 'hand_pump', ['precision_tools','leather','fittings','joinery'])
rule('drive_pump', 'powered_pump', ['hand_pump'], [['water_power','wind_power']])
rule('fit_plank_hull', 'plank_boat', ['precision_tools','joinery','fittings','cordage'], note='Caulking/sealing and displacement require detailed vehicle recipes and simulation.')
doc = dict(schema_version=1, status='design_proposal_not_runtime_recipes', date='2026-09-06',
           endpoint='steel_water_wind_advanced_workshops', voxel_edge_m=0.025,
           semantics={'requires_all':'AND of node availability',
                      'requires_any_groups':'AND across groups; OR within each nonempty group',
                      'rules':'Alternative rules granting the same capability are OR routes.',
                      'availability':'Existential reachability, not inventory consumption or permanent entitlement.',
                      'limits':'Assumes sufficient accessible resources; does not validate quantities, geography, process physics or balance.'},
           resources=[dict(id=k,label=v) for k,v in resources.items()],
           capabilities=[dict(id=k,family=v[0],label=v[1]) for k,v in caps.items()], rules=rules)
(out/'crafting-capabilities-v1.json').write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')
print(f'Wrote {len(resources)} resources, {len(caps)} capabilities, {len(rules)} production methods.')

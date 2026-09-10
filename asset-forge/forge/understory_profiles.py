"""Explicit understory review scope and botanical source identities.

Architecture names describe morphology, not a claim of completed review.
"""
VERSION='temperate-understory-v1'
# name | taxon | architecture
_ROWS='''arrowhead|Sagittaria latifolia|arrow
bilberry-mat|Vaccinium myrtillus|dwarf
birdsfoot-trefoil|Lotus corniculatus|trifoliate
blackthorn-scrub|Prunus spinosa|shrub
blanket-weed|Cladophora glomerata|mat
bogbean|Menyanthes trifoliata|trifoliate
box|Buxus sempervirens|evergreen
bracken|Pteridium aquilinum|fern
bramble-thicket|Rubus fruticosus|cane
branched-bur-reed|Sparganium erectum|burreed
broadleaf-cattail|Typha latifolia|cattail
brook-moss-cushion|Brachythecium rivulare|moss
bugle|Ajuga reptans|spike
bulrush|Schoenoplectus lacustris|rush
butchers-broom|Ruscus aculeatus|evergreen
canadian-waterweed|Elodea canadensis|submerged
cocksfoot|Dactylis glomerata|grass
common-bluebell|Hyacinthoides non-scripta|bells
common-broom|Cytisus scoparius|broom
common-cottongrass|Eriophorum angustifolium|cotton
common-dog-violet|Viola riviniana|rosette
common-dogwood|Cornus sanguinea|shrub
common-knapweed|Centaurea nigra|rosette
common-milkweed|Asclepias syriaca|opposite
common-poppy|Papaver rhoeas|rosette
common-stonewort|Chara vulgaris|submerged
cowslip|Primula veris|umbel
curled-pondweed|Potamogeton crispus|submerged
cyclamen|Cyclamen hederifolium|rosette
dog-rose|Rosa canina|cane
dogs-mercury|Mercurialis perennis|opposite
elder|Sambucus nigra|compound
feather-moss|Pleurozium schreberi|moss
field-scabious|Knautia arvensis|rosette
fireweed|Chamaenerion angustifolium|spike
flowering-rush|Butomus umbellatus|umbel
foxglove|Digitalis purpurea|bells
fringed-water-lily|Nymphoides peltata|floating
gorse|Ulex europaeus|broom
guelder-rose|Viburnum opulus|shrub
hair-cap-moss|Polytrichum commune|moss
harebell|Campanula rotundifolia|bells
harts-tongue-fern|Asplenium scolopendrium|strap
hazel-coppice|Corylus avellana|coppice
hellebore|Helleborus orientalis|palmate
herb-robert|Geranium robertianum|palmate
holly-understorey|Ilex aquifolium|evergreen
impatiens|Impatiens capensis|opposite
ivy-ground-layer|Hedera helix|runner
jungle-groundcover|Luzula sylvatica|strap
jungle-understory-flower|Maianthemum racemosum|panicleherb
lady-fern|Athyrium filix-femina|fern
large-trillium|Trillium grandiflorum|trillium
lesser-pond-sedge|Carex acutiformis|sedge
lily-of-the-valley|Convallaria majalis|bells
male-fern|Dryopteris filix-mas|fern
marsh-cinquefoil|Comarum palustre|palmate
marsh-marigold|Caltha palustris|rosette
meadow-daisy|Bellis perennis|rosette
meadow-grass|Poa pratensis|grass
moss-cushion|Leucobryum glaucum|moss
mountain-laurel|Kalmia latifolia|evergreen
needle-spike-rush|Eleocharis acicularis|rush
oxeye-daisy|Leucanthemum vulgare|rosette
pickerelweed|Pontederia cordata|arrow
prairie-lupine|Lupinus perennis|palmate
primrose|Primula vulgaris|rosette
purple-loosestrife|Lythrum salicaria|spike
quillwort|Isoetes lacustris|strap
ramsons|Allium ursinum|umbel
red-campion|Silene dioica|opposite
red-clover|Trifolium pratense|trifoliate
red-huckleberry|Vaccinium parvifolium|dwarf
reed-sweet-grass|Glyceria maxima|grass
rhododendron-thicket|Rhododendron ponticum|evergreen
ribwort-plantain|Plantago lanceolata|rosette
rigid-hornwort|Ceratophyllum demersum|submerged
river-water-crowfoot|Ranunculus fluitans|submerged
salal|Gaultheria shallon|evergreen
sedge-tussock|Carex cespitosa|sedge
snowberry|Symphoricarpos albus|shrub
soft-rush|Juncus effusus|rush
sphagnum-hummock|Sphagnum palustre|moss
spiked-water-milfoil|Myriophyllum spicatum|submerged
spindle|Euonymus europaeus|shrub
sweet-flag|Acorus calamus|strap
sword-fern|Polystichum munitum|fern
timothy|Phleum pratense|grass
trout-lily|Erythronium americanum|bells
understory-fern|Dryopteris filix-mas|fern
water-chestnut|Trapa natans|floating
water-earwort|Salvinia natans|floating
water-forget-me-not|Myosotis scorpioides|opposite
water-horsetail|Equisetum fluviatile|horsetail
water-mint|Mentha aquatica|opposite
water-plantain|Alisma plantago-aquatica|umbel
water-reed|Phragmites australis|reed
water-soldier|Stratiotes aloides|strap
water-starwort|Callitriche stagnalis|floating
watercress|Nasturtium officinale|runner
white-water-lily|Nymphaea alba|floating
wild-privet|Ligustrum vulgare|shrub
wild-rice|Zizania aquatica|reed
willow-moss|Fontinalis antipyretica|submerged
witch-hazel|Hamamelis virginiana|shrub
wood-anemone|Anemone nemorosa|palmate
wood-horsetail|Equisetum sylvaticum|horsetail
wood-sedge|Carex sylvatica|sedge
wood-sorrel|Oxalis acetosella|trifoliate
woolly-fringe-moss|Racomitrium lanuginosum|moss
yarrow|Achillea millefolium|compoundherb
yellow-flag-iris|Iris pseudacorus|strap
yellow-water-lily|Nuphar lutea|floating'''
PROFILES={name:dict(taxon=taxon,architecture=arch) for name,taxon,arch in (r.split('|') for r in _ROWS.splitlines())}
# Flowers outside spring flowering season retain their vegetative architecture.
SPRING_FLOWERS=set('arrowhead birdsfoot-trefoil bogbean bugle common-bluebell common-broom common-dog-violet cowslip foxglove flowering-rush gorse hellebore herb-robert large-trillium lily-of-the-valley marsh-marigold meadow-daisy oxeye-daisy primrose ramsons red-campion red-clover trout-lily water-forget-me-not wood-anemone wood-sorrel yellow-flag-iris rhododendron-thicket mountain-laurel'.split())

# Initial species-scale controls; acceptance still requires photographed pilots.
_SHRUB_LEAVES={
 'blackthorn-scrub':(.045,.35),'box':(.035,.32),'bramble-thicket':(.10,.42),
 'butchers-broom':(.065,.32),'common-broom':(.035,.15),'common-dogwood':(.085,.35),
 'dog-rose':(.075,.35),'elder':(.14,.30),'gorse':(.025,.10),'guelder-rose':(.10,.42),
 'hazel-coppice':(.12,.46),'holly-understorey':(.085,.31),'mountain-laurel':(.10,.30),
 'red-huckleberry':(.045,.35),'rhododendron-thicket':(.15,.26),'salal':(.10,.36),
 'snowberry':(.055,.36),'spindle':(.085,.28),'wild-privet':(.065,.28),
 'witch-hazel':(.11,.42),'bilberry-mat':(.04,.34)}
for _name,(_length,_width) in _SHRUB_LEAVES.items():
 PROFILES[_name].update(leaf_length=_length,leaf_width=_width)
PROFILES['red-huckleberry'].update(green_stems=True,fine_forks=True,spread=1.2,stem_radius=.01)
PROFILES['bilberry-mat'].update(green_stems=True,fine_forks=True,stems_min=3,stems_max=5,fork_twigs=1,leaves_per_shoot=3,leaf_length=.025,spread=1.3)
PROFILES['common-broom'].update(green_stems=True,stem_radius=.012,spread=.75)
PROFILES['gorse'].update(green_stems=True,stem_radius=.012,spread=.85)
PROFILES['box'].update(stems_min=8,stems_max=12,spread=1.2,leaf_roundness=.5)
PROFILES['primrose'].update(leaf_roundness=.45)
PROFILES['common-dog-violet'].update(leaf_roundness=.4)
PROFILES['sword-fern'].update(frond_open_min=.25,frond_open_max=.62)
PROFILES['lady-fern'].update(frond_open_min=.4,frond_open_max=.85)

PROFILES['box'].update(fork_twigs=5,leaves_per_shoot=9,green_twigs=True,twig_radius=.003,stem_radius=.015)
PROFILES['bramble-thicket'].update(compound='palmate',green_twigs=True,twig_radius=.004,stem_radius=.012,spread=2.,stems_min=3,stems_max=5,fork_twigs=1,leaves_per_shoot=3)
PROFILES['dog-rose'].update(compound='pinnate',green_twigs=True,twig_radius=.004,stem_radius=.016,spread=1.1)
PROFILES['harts-tongue-fern'].update(blade_width_fraction=.045)
PROFILES['sword-fern'].update(simple_pinnae=True,pinna_pairs=15,frond_width=.13,fronds_min=10,fronds_max=15)
PROFILES['bracken'].update(triangular_frond=True,bare_stipe=.32,frond_width=.45,fronds_min=3,fronds_max=6)
PROFILES['jungle-groundcover'].update(architecture='woodrush',display_name='Great wood-rush',baseline_height_m=.45,blade_width_fraction=.018,spread=1.4,reference_role='Chosen temperate woodland replacement for the explicitly listed generic shade-floor placeholder; legacy ID retained')
PROFILES['jungle-understory-flower'].update(display_name="False Solomon's seal",head_material='plume_white',spread=2.,reference_role='Chosen temperate woodland replacement for the explicitly listed generic tall shade-bloom placeholder; legacy ID retained')
SPRING_FLOWERS.add('jungle-understory-flower')
PROFILES['ramsons'].update(architecture='wildgarlic',spread=1.5)
PROFILES['wood-anemone'].update(architecture='anemone',spread=2.,baseline_height_m=.22)
PROFILES['wood-sorrel'].update(architecture='oxalis',spread=2.,baseline_height_m=.12)
PROFILES['lily-of-the-valley'].update(architecture='convallaria',spread=1.6,baseline_height_m=.28)
PROFILES['cowslip'].update(architecture='cowslip',baseline_height_m=.30,leaf_roundness=.45)
PROFILES['common-dog-violet'].update(architecture='violet',baseline_height_m=.10,spread=2.)
PROFILES['feather-moss'].update(architecture='feathermoss',baseline_height_m=.08,spread=3.)
PROFILES['hair-cap-moss'].update(architecture='haircap',spread=1.8)
PROFILES['sphagnum-hummock'].update(architecture='peatmoss',baseline_height_m=.20,spread=1.8)
PROFILES['blackthorn-scrub'].update(fork_twigs=4,leaves_per_shoot=8,twig_radius=.004,spread=1.2)
PROFILES['salal'].update(stems_min=3,stems_max=5,fork_twigs=1,leaves_per_shoot=4,spread=1.8,leaf_roundness=.5,twig_radius=.004,stem_radius=.015)
PROFILES['common-dogwood'].update(opposite_leaves=True,leaves_per_shoot=4,spread=1.4,stem_radius=.018,twig_radius=.004)
for _name in ('spindle','wild-privet','snowberry','guelder-rose'):
 PROFILES[_name].update(opposite_leaves=True,leaves_per_shoot=4)
PROFILES['spindle'].update(green_twigs=True,twig_radius=.003,stem_radius=.018,spread=1.4)
PROFILES['wild-privet'].update(twig_radius=.003,stem_radius=.015,spread=1.5,fork_twigs=3,leaves_per_shoot=6,leaf_length=.06,leaf_width=.22)
PROFILES['snowberry'].update(twig_radius=.003,stem_radius=.012,spread=1.8,fork_twigs=2,leaf_roundness=.5,leaf_length=.045,leaf_width=.4)
PROFILES['guelder-rose'].update(architecture='viburnum',spread=1.6,head_material='plume_white')
PROFILES['witch-hazel'].update(spread=1.6,stems_min=3,stems_max=5,leaf_length=.11,leaf_width=.4,leaf_roundness=.45,twig_radius=.004,stem_radius=.02)
PROFILES['butchers-broom'].update(green_stems=True,green_twigs=True,stem_radius=.005,twig_radius=.002,leaf_length=.04,leaf_width=.3,stems_min=9,stems_max=14,spread=1.8,fork_twigs=1,leaves_per_shoot=3)
PROFILES['common-broom'].update(architecture='scoparius',spread=1.4,head_material='skin_yellow')
PROFILES['gorse'].update(architecture='ulex',spread=1.6,head_material='skin_yellow')
PROFILES['rhododendron-thicket'].update(architecture='ponticum',spread=1.7,head_material='plume_lilac')
PROFILES['mountain-laurel'].update(architecture='kalmia',spread=1.5,head_material='plume_white')
PROFILES['trout-lily'].update(architecture='erythronium',spread=2.,baseline_height_m=.22)
PROFILES['foxglove'].update(architecture='digitalis',spread=1.3)
PROFILES['hellebore'].update(architecture='helleborus',spread=1.8,head_material='plume_white')
PROFILES['bogbean'].update(architecture='menyanthes',spread=2.,baseline_height_m=.35)
PROFILES['marsh-marigold'].update(architecture='caltha',spread=2.,baseline_height_m=.4)
PROFILES['herb-robert'].update(architecture='geranium',spread=2.,baseline_height_m=.35)
PROFILES['red-campion'].update(architecture='silene',spread=1.7)
PROFILES['bugle'].update(architecture='ajuga',spread=2.,baseline_height_m=.25,head_material='plume_lilac')

PROFILES['cocksfoot'].update(architecture='dactylis',baseline_height_m=.65,spread=1.3,basal_blades=32,culms=7,blade_halfwidth=.004,max_blade=.4)
PROFILES['timothy'].update(architecture='phleum',baseline_height_m=.7,spread=1.25,basal_blades=18,culms=7,blade_halfwidth=.003,max_blade=.3)
PROFILES['reed-sweet-grass'].update(architecture='glyceria',baseline_height_m=1.3,spread=1.5,basal_blades=26,culms=10,blade_halfwidth=.006,max_blade=.6)
PROFILES['common-cottongrass'].update(architecture='eriophorum',baseline_height_m=.45,spread=1.4,basal_blades=24,culms=5,blade_halfwidth=.002,max_blade=.25)
PROFILES['soft-rush'].update(architecture='juncus',baseline_height_m=.8,spread=1.1)
PROFILES['water-horsetail'].update(architecture='fluviatile',baseline_height_m=.8,spread=1.7)
PROFILES['wood-horsetail'].update(architecture='sylvaticum',baseline_height_m=.45,spread=2.0)

PROFILES['wood-sedge'].update(architecture='carexsylvatica',baseline_height_m=.6,spread=1.4)
PROFILES['brook-moss-cushion'].update(architecture='rivulare',baseline_height_m=.10,spread=3.)
PROFILES['woolly-fringe-moss'].update(architecture='lanuginosum',baseline_height_m=.08,spread=3.)

PROFILES['birdsfoot-trefoil'].update(architecture='lotus',baseline_height_m=.25,spread=2.)
PROFILES['red-clover'].update(architecture='trifolium',baseline_height_m=.35,spread=1.8)
PROFILES['meadow-daisy'].update(architecture='bellis',baseline_height_m=.15,spread=2.,head_material='plume_white')
PROFILES['oxeye-daisy'].update(architecture='leucanthemum',baseline_height_m=.65,spread=1.6)
PROFILES['ribwort-plantain'].update(architecture='plantago',baseline_height_m=.45,spread=1.6)
PROFILES['cyclamen'].update(architecture='hederifolium',baseline_height_m=.15,spread=2.)
PROFILES['prairie-lupine'].update(architecture='lupinus',baseline_height_m=.6,spread=1.7,head_material='plume_lilac')
PROFILES['yarrow'].update(architecture='achillea',baseline_height_m=.35,spread=2.)
SPRING_FLOWERS.add('prairie-lupine')

PROFILES['yellow-water-lily'].update(architecture='nuphar',baseline_height_m=.05,spread=3.)
PROFILES['water-earwort'].update(architecture='salvinia',baseline_height_m=.05,spread=3.)
PROFILES['blanket-weed'].update(architecture='cladophora',baseline_height_m=.10,spread=3.)
PROFILES['common-stonewort'].update(architecture='chara',baseline_height_m=.35,spread=2.)
PROFILES['river-water-crowfoot'].update(architecture='fluitans',baseline_height_m=.12,trailing_length_m=.9,spread=2.)
PROFILES['sedge-tussock'].update(architecture='carexcespitosa',baseline_height_m=.45,spread=1.4)

PROFILES['common-knapweed'].update(architecture='centaurea',baseline_height_m=.4,spread=1.8)
PROFILES['field-scabious'].update(architecture='knautia',baseline_height_m=.35,spread=2.)
PROFILES['common-poppy'].update(architecture='papaver',baseline_height_m=.6,spread=1.7,head_material='skin_red')
PROFILES['harebell'].update(architecture='campanula',baseline_height_m=.18,spread=2.)
PROFILES['common-milkweed'].update(architecture='asclepias',baseline_height_m=.65,spread=1.8,leaf_m=.2,leaf_width_m=.04)
PROFILES['fireweed'].update(architecture='chamaenerion',baseline_height_m=.55,spread=1.6,leaf_m=.13,leaf_width_m=.009)
PROFILES['impatiens'].update(architecture='impatiens',baseline_height_m=.4,spread=2.,leaf_m=.09,leaf_width_m=.025)
PROFILES['purple-loosestrife'].update(architecture='lythrum',baseline_height_m=.55,spread=1.8,leaf_m=.10,leaf_width_m=.012)
PROFILES['marsh-cinquefoil'].update(architecture='comarum',baseline_height_m=.3,spread=2.2)
PROFILES['water-mint'].update(architecture='mentha',baseline_height_m=.3,spread=1.8,leaf_m=.05,leaf_width_m=.015)
PROFILES['water-chestnut'].update(architecture='trapa',baseline_height_m=.05,spread=3.)
PROFILES['water-starwort'].update(architecture='callitriche',baseline_height_m=.05,spread=3.)
PROFILES['fringed-water-lily'].update(architecture='nymphoides',baseline_height_m=.05,spread=3.)
PROFILES['curled-pondweed'].update(architecture='potamogeton',baseline_height_m=.35,spread=2.)
SPRING_FLOWERS.difference_update({'common-knapweed','field-scabious','harebell','common-milkweed','fireweed','impatiens','purple-loosestrife','marsh-cinquefoil','water-mint','fringed-water-lily','arrowhead','flowering-rush'})

PROFILES['arrowhead'].update(architecture='sagittaria',baseline_height_m=.65,spread=1.7)
PROFILES['water-plantain'].update(architecture='alisma',baseline_height_m=.5,spread=1.8)
PROFILES['pickerelweed'].update(architecture='pontederia',baseline_height_m=.65,spread=1.7)
PROFILES['branched-bur-reed'].update(architecture='sparganium',baseline_height_m=.9,spread=1.5,blade_halfwidth=.012)
PROFILES['flowering-rush'].update(architecture='butomus',baseline_height_m=.9,spread=1.5,blade_halfwidth=.004)
PROFILES['sweet-flag'].update(architecture='acorus',baseline_height_m=.8,spread=1.5,blade_halfwidth=.009)
PROFILES['yellow-flag-iris'].update(architecture='pseudacorus',baseline_height_m=.9,spread=1.5,blade_halfwidth=.016,head_material='skin_yellow')
PROFILES['bulrush'].update(architecture='schoenoplectus',baseline_height_m=1.5,spread=1.1)
PROFILES['needle-spike-rush'].update(architecture='eleocharis',baseline_height_m=.18,spread=2.2)
PROFILES['lesser-pond-sedge'].update(architecture='acutiformis',baseline_height_m=.8,spread=1.5)
PROFILES['water-reed'].update(architecture='phragmites',baseline_height_m=1.5,spread=1.4,leaf_m=.5,leaf_halfwidth=.018)
PROFILES['wild-rice'].update(architecture='zizania',baseline_height_m=.9,spread=1.6,leaf_m=.4,leaf_halfwidth=.012)
PROFILES['ivy-ground-layer'].update(architecture='hedera',baseline_height_m=.12,spread=3.)
PROFILES['willow-moss'].update(architecture='fontinalis',baseline_height_m=.15,spread=3.)
PROFILES['water-forget-me-not'].update(architecture='myosotis',baseline_height_m=.25,spread=2.)
PROFILES['watercress'].update(architecture='nasturtium',baseline_height_m=.3,spread=2.2,head_material='plume_white')
PROFILES['canadian-waterweed'].update(architecture='elodea',baseline_height_m=.35,spread=2.)
PROFILES['rigid-hornwort'].update(architecture='ceratophyllum',baseline_height_m=.35,spread=2.)
PROFILES['spiked-water-milfoil'].update(architecture='myriophyllum',baseline_height_m=.4,spread=2.)
PROFILES['quillwort'].update(architecture='isoetes',baseline_height_m=.23,spread=1.6)
PROFILES['water-soldier'].update(architecture='stratiotes',baseline_height_m=.3,spread=2.)
PROFILES['white-water-lily'].update(architecture='nymphaea',baseline_height_m=.05,spread=3.)
SPRING_FLOWERS.difference_update({'water-forget-me-not','water-plantain','pickerelweed','white-water-lily','water-soldier'})
SPRING_FLOWERS.update({'yellow-flag-iris','watercress'})

PROFILES['needle-spike-rush'].update(spread=3.)

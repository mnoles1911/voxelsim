"""Temperate collection art direction. Metric proportions are authored game targets.

Sources and habitat roles are recorded per species; these are not spawn weights.
Reference-reviewed recipes remain candidates until the owner keeps their outputs.
"""
from dataclasses import dataclass

ART_BASELINE = {
    'season': 'spring',
    'foliage': 'Species-appropriate spring foliage and fresh growth; flowering only where appropriate to the species.',
    'reference_policy': 'Use spring photographs for finished color and foliage. Other seasons may inform branch structure only.',
    'out_of_scope': ['seasonal transitions', 'seasonal recoloring'],
}

@dataclass(frozen=True)
class Profile:
    taxon: str
    architecture: str
    width: float
    clear: float
    rise: float
    droop: float
    leaf: float
    density: float
    bark: str
    habitat: str
    cue: str

# width/clear/rise/droop are relative to height. leaf is a shoot-group length,
# not the size of an individual botanical leaf at this coarse voxel pitch.
PROFILES = {
    'american-beech': Profile('fagus-grandifolia','layered',.88,.22,.12,.025,.48,1.15,'smooth','canopy','Low spreading limbs; alternate leaves on horizontal branchlets.'),
    'european-beech': Profile('fagus-sylvatica','layered',.82,.20,.16,.015,.42,1.25,'smooth','canopy','Dense oval crown; long low limbs; smooth bole.'),
    'sugar-maple': Profile('acer-saccharum','opposite',.72,.30,.26,0,.48,1.15,'rough','canopy','Opposite forks and leaf fans; upright oval crown.'),
    'bigleaf-maple': Profile('acer-macrophyllum','opposite',.98,.22,.20,.015,.70,1.0,'rough','coastal','Broad heavy forks and larger palmate leaf groups.'),
    'sycamore-maple': Profile('acer-pseudoplatanus','opposite',.84,.27,.23,0,.55,1.1,'rough','canopy','Broad dome and robust opposite forks.'),
    'field-maple': Profile('acer-campestre','opposite',.86,.24,.22,0,.37,1.05,'rough','edge','Compact low forks with small leaf groups.'),
    'japanese-maple': Profile('acer-palmatum','layered',1.18,.22,.10,.018,.34,1.05,'smooth','understory-regional','Low spreading layered crown; opposite twig fans.'),
    'hornbeam': Profile('carpinus-betulus','oval',.66,.27,.24,0,.40,1.25,'fluted','canopy','Dense upright crown and fluted trunk; fine alternate twigs.'),
    'small-leaved-lime': Profile('tilia-cordata','oval',.70,.23,.27,.01,.39,1.2,'rough','canopy','Heart-shaped crown profile with ascending upper limbs.'),
    'common-ash': Profile('fraxinus-excelsior','compound',.83,.34,.28,0,.54,.82,'rough','canopy','Open crown; ascending opposite limbs and pinnate sprays.'),
    'field-elm': Profile('ulmus-minor','vase',.84,.29,.34,.035,.40,1.05,'rough','edge','Ascending divided scaffolds with arching outer twigs.'),
    'wych-elm': Profile('ulmus-glabra','vase',1.0,.25,.30,.04,.49,1.12,'rough','canopy','Broad spreading dome over strong ascending forks.'),
    'birch': Profile('betula-pendula','light',.55,.29,.28,.065,.32,.80,'pale','pioneer','Slender pale bole, loose narrow crown, hanging fine twigs.'),
    'common-alder': Profile('alnus-glutinosa','oval',.60,.26,.22,.015,.40,1.0,'rough','wetland','Conical young crown and irregular mature lateral branches.'),
    'eastern-cottonwood': Profile('populus-deltoides','vase',.88,.34,.30,0,.52,.98,'rough','wetland','Large ascending forks and broad open crown.'),
    'quaking-aspen': Profile('populus-tremuloides','light',.42,.40,.25,0,.34,.88,'pale','pioneer','Straight pale stem; small narrow rounded crown.'),
    'white-poplar': Profile('populus-alba','oval',.70,.30,.26,.02,.43,.98,'pale','wetland','Broad irregular crown on pale branching stem.'),
    'weeping-willow': Profile('salix-babylonica','weeping',1.10,.22,.22,.27,.38,1.15,'rough','wetland','Arched scaffold with long downward leafy streamers.'),
    'black-cherry': Profile('prunus-serotina','oval',.60,.38,.27,0,.40,.96,'rough','canopy','Tall clear trunk and irregular oval crown; fine alternate shoots.'),
    'cherry-blossom': Profile('prunus-serrulata','vase',1.06,.25,.25,.025,.38,1.05,'smooth','understory-regional','Spreading ascending branches with blossom accents on shoots.'),
    'flowering-dogwood': Profile('cornus-florida','layered',1.10,.24,.06,.012,.44,1.02,'rough','understory','Horizontal tiered branches; opposite leaf fans.'),
    'hawthorn-scrub': Profile('crataegus-monogyna','scrub',1.15,.15,.20,0,.28,1.25,'rough','edge','Low crooked fork network and dense small shoots.'),
    'crab-apple': Profile('malus-sylvestris','scrub',1.05,.23,.24,.01,.34,1.1,'rough','edge','Low irregular spreading forks; short leafy spur shoots.'),
    'wild-pear': Profile('pyrus-pyraster','oval',.64,.24,.34,0,.35,1.1,'rough','edge','More upright spurred crown than crab apple.'),
    'rowan': Profile('sorbus-aucuparia','compound',.73,.28,.30,.01,.46,.85,'smooth','edge','Light ascending crown and compound leaf sprays.'),
    'shagbark-hickory': Profile('carya-ovata','compound',.54,.40,.25,.02,.61,.95,'shaggy','canopy','Tall bole, narrow open crown, large compound sprays and bark strips.'),
    'sweet-chestnut': Profile('castanea-sativa','vase',.86,.26,.28,.015,.61,1.05,'rough','canopy','Strong spreading forks with elongated leaf fans.'),
    'tulip-tree': Profile('liriodendron-tulipifera','oval',.48,.43,.27,0,.52,1.02,'rough','canopy','Straight dominant leader, tall clear bole and high crown.'),
    'honey-locust': Profile('gleditsia-triacanthos','compound',.90,.31,.24,.025,.38,.72,'rough','edge','Open irregular scaffold with fine pinnate sprays.'),
    'bur-oak': Profile('quercus-macrocarpa','oak',1.20,.18,.23,0,.54,1.05,'rough','edge','Heavy low spreading oak limbs and irregular broad crown.'),
    'cork-oak': Profile('quercus-suber','oak',1.08,.23,.22,0,.33,1.1,'rough','warm-dry','Low evergreen oak crown; thick cork-like bole.'),
    'holm-oak': Profile('quercus-ilex','oak',.95,.21,.24,0,.32,1.22,'rough','warm-dry','Dense evergreen rounded crown, compact leaf sprays.'),
    'temperate-oak': Profile('quercus-robur','oak',1.22,.22,.23,0,.48,1.1,'rough','canopy','Existing reference oak branch-and-shoot architecture.'),
    'river-broadleaf': Profile('alnus-glutinosa','vase',1.03,.20,.28,.025,.48,1.05,'rough','wetland','Authored riparian archetype, using alder reference; spreading riverbank forks.'),
    'temperate-sapling': Profile('acer-campestre','opposite',.51,.25,.28,0,.32,.85,'smooth','regeneration','Authored juvenile archetype; few leader forks and sparse lower branches.'),
    'douglas-fir': Profile('pseudotsuga-menziesii','conifer',.36,.27,.025,.022,.54,1.05,'rough','coastal','Dominant spire, descending lower boughs, raised old woodland crown.'),
    'eastern-hemlock': Profile('tsuga-canadensis','hemlock',.49,.18,.012,.052,.48,1.1,'rough','cool-moist','Horizontal flattened sprays and pendant branch tips.'),
    'western-hemlock': Profile('tsuga-heterophylla','hemlock',.40,.23,.01,.065,.51,1.1,'rough','coastal','Narrow spire with drooping leader and lacy hanging sprays.'),
    'western-red-cedar': Profile('thuja-plicata','cedar',.40,.16,.015,.048,.59,1.15,'fluted','coastal','Flared bole and broad flattened descending scale-leaf fans.'),
    'norway-spruce': Profile('picea-abies','spruce',.40,.17,.018,.075,.45,1.08,'rough','cool','Tiered boughs with pendant secondary branchlets.'),
    'sitka-spruce': Profile('picea-sitchensis','spruce',.36,.23,.035,.027,.50,1.12,'rough','coastal','Tall spire with broad stiff lateral boughs.'),
    'scots-pine': Profile('pinus-sylvestris','pine',.64,.42,.18,.012,.46,.92,'rough','cool-dry','Sparse irregular mature crown; needles concentrated on terminal shoots.'),
    'maritime-pine': Profile('pinus-pinaster','pine',.70,.40,.16,.015,.65,1.0,'rough','warm-coastal','High irregular crown with coarser terminal needle tufts.'),
    'tundra-pine': Profile('pinus-sylvestris','pine',.51,.22,.14,.025,.43,.95,'rough','cool-transition','Authored cold-edge pine archetype; low uneven wind-exposed crown.'),
    'monterey-cypress': Profile('hesperocyparis-macrocarpa','cedar-wide',1.02,.29,.12,.025,.52,1.12,'rough','coastal-regional','Broad wind-shaped branching canopy with scale-leaf fans.'),
    'columnar-cypress': Profile('cupressus-sempervirens','column',.17,.08,.22,0,.36,1.25,'rough','warm-regional','Narrow upright branches close to the leader; columnar silhouette.'),
    'european-yew': Profile('taxus-baccata','cedar-wide',.88,.16,.13,.01,.36,1.18,'fluted','understory','Broad low evergreen crown with dense flat needle sprays.'),
    'hero-sequoia': Profile('sequoiadendron-giganteum','giant',.30,.30,.025,.015,.60,1.12,'fluted','montane-hero','Rare giant archetype; massive tapered bole and high irregular boughs.'),
    'tree-fern': Profile('dicksonia-antarctica','fern',1.03,.75,.16,.15,.45,1.0,'rough','wet-mild-regional','Single fibrous stem and arching divided fronds from its crown.'),
}

SOURCE_OVERRIDES = {
    'fraxinus-excelsior': 'https://www.rhs.org.uk/plants/7308/fraxinus-excelsior/details',
    'crataegus-monogyna': 'https://www.woodlandtrust.org.uk/trees-woods-and-wildlife/british-trees/a-z-of-british-trees/hawthorn/',
    'pyrus-pyraster': 'https://www.rhs.org.uk/plants/69543/pyrus-pyraster/details',
    'quercus-ilex': 'https://www.treesandshrubsonline.org/articles/quercus/quercus-ilex/',
    'hesperocyparis-macrocarpa': 'https://www.conifers.org/cu/Hesperocyparis_macrocarpa.php',
    'dicksonia-antarctica': 'https://www.rhs.org.uk/plants/5794/dicksonia-antarctica/details/',
}

def source(profile):
    return SOURCE_OVERRIDES.get(profile.taxon, f'https://plants.ces.ncsu.edu/plants/{profile.taxon}/')

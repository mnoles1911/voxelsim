"""Metric understory architectures. Recipes stay opt-in until source acceptance.

Leaves follow explicit petioles, fronds follow rachises, and flowers attach to
pedicels. No spherical foliage volumes are used. All dimensions are metres.
"""
import hashlib,math,time
import numpy as np
from . import materials as M
from .grid import VoxelGrid
from .understory_profiles import PROFILES,VERSION,SPRING_FLOWERS
SIZES=('small','medium','large')
FORMS=('compact','spreading','leaning')
def validate_recipe(recipe,kind):
 if kind not in ('bush','reed','grass','flower') or set(recipe)-{'generator','profile','size','form'}:raise ValueError('Invalid understory recipe')
 if recipe.get('generator')!=VERSION or recipe.get('profile') not in PROFILES:raise ValueError('Unknown understory profile')
 if PROFILES[recipe['profile']]['architecture']=='unresolved':raise ValueError('Profile requires botanical/habitat review')
 size,form=recipe.get('size','mixed'),recipe.get('form','mixed')
 if size not in (*SIZES,'mixed') or form not in (*FORMS,'mixed'):raise ValueError('Invalid understory size/form')
 return dict(generator=VERSION,profile=recipe['profile'],size=size,form=form)
def unit(v):
 v=np.array(v,float);return v/max(1e-8,np.linalg.norm(v))
def build(spec,seed,pitch,connectivity=True):
 from .pipeline import Asset
 from .spec import spec_hash,get
 from scipy import ndimage
 start=time.perf_counter();r=validate_recipe(spec['plant_recipe'],spec['kind']);p=PROFILES[r['profile']];arch=p['architecture']
 size=r['size'] if r['size']!='mixed' else SIZES[((seed-1)//3)%3]
 form=r['form'] if r['form']!='mixed' else FORMS[(seed-1)%3]
 rng=np.random.default_rng([seed,int.from_bytes(hashlib.sha256(r['profile'].encode()).digest()[:4],'little')])
 h=float(spec['height_m'])*{'small':.55,'medium':.78,'large':1.}[size]*rng.uniform(.90,1.08)
 spread=h*{'compact':.27,'spreading':.52,'leaning':.36}[form]*p.get('spread',1.)
 segments=[];leafcount=0
 green=M.MAT_LEAF_BROADLEAF;wood=M.MAT_BARK;stemmat=green if p.get('green_stems') else wood
 head=M.resolve(get(spec,'materials.head'))
 def line(a,b,width=.012,mat=green):
  segments.append((np.array(a,float),np.array(b,float),width,mat))
 def curve(a,b,arc=.0,width=.012,mat=green):
  a,b=np.array(a,float),np.array(b,float);ts=np.linspace(0,1,9)
  ps=np.array([a+(b-a)*t+[0,0,arc*math.sin(math.pi*t)] for t in ts]);ps[:,2]=np.maximum(.015,ps[:,2])
  for j,(x,y) in enumerate(zip(ps[:-1],ps[1:])):line(x,y,width*(1-.78*j/8) if mat==wood else width,mat)
  return ps
 def leaf(a,d,length,width):
  nonlocal leafcount
  d=unit(d);cross=unit(np.cross(d,[0,0,1]))
  if np.linalg.norm(cross)<.1:cross=np.array([1.,0,0])
  a=np.array(a,float)
  for t in np.linspace(0,1,max(4,int(length/pitch)+1)):
   center=a+d*length*t+[0,0,length*.1*math.sin(math.pi*t)]
   w=width*max(0,math.sin(math.pi*t))**p.get('leaf_roundness',.8)
   line(center-cross*w,center+cross*w,.007)
  line(a,a+d*length,.008);leafcount+=1
 def bloom(a,scale=1):
  # A coherent whorl of attached petals; not scattered coloured canopy cells.
  a=np.array(a,float);rad=max(pitch*.9,min(.055,h*.075))*scale
  for angle in np.linspace(0,math.tau,5,endpoint=False):
   line(a,a+[math.cos(angle)*rad,math.sin(angle)*rad,-rad*.10],rad*.45,head)
 def shoot(a,b,compound=False):
  ps=curve(a,b,h*.025,p.get('twig_radius',.009),stemmat if spec['kind']=='bush' and not p.get('green_twigs') else green)
  for j,t in enumerate(np.linspace(.23,.94,p.get('leaves_per_shoot',5))):
   at=ps[min(7,int(t*8))];theta=j*2.4+rng.uniform(-.2,.2)
   d=unit([math.cos(theta),math.sin(theta),rng.uniform(-.25,.8)]);length=min(p.get('leaf_length',.18),h*.18)*rng.uniform(.8,1.2)
   if compound or p.get('compound'):
    end=at+d*length;line(at,end)
    side=np.array([-d[1],d[0],.2])
    if p.get('compound')=='palmate':
     for sign in (-1,0,1):leaf(end,d+side*sign,length*.75,length*.26)
    else:
     for k in range(1,4):
      for sign in (-1,1):leaf(at+d*length*k/4,d*.5+side*sign,length*.4,length*.13)
     leaf(end,d,length*.45,length*.14)
   elif p.get('opposite_leaves'):
    for sign in (-1,1):leaf(at,[d[0]*sign,d[1]*sign,d[2]],length,length*p.get('leaf_width',.35))
   else:leaf(at,d,length,length*p.get('leaf_width',.27 if arch=='evergreen' else .40))
  return ps
 root=np.array([0.,0.,.015])
 woody=arch in ('shrub','evergreen','coppice','cane','compound','dwarf','broom')
 if woody:
  n=int(rng.integers(p.get('stems_min',5),p.get('stems_max',8)+1));height=h
  for i in range(n):
   theta=i*math.tau/n+rng.uniform(-.3,.3);d=np.array([math.cos(theta),math.sin(theta),0])
   end=d*spread*rng.uniform(.5,1.0)+[h*.10 if form=='leaning' else 0,0,h*rng.uniform(.7,1)]
   base=root+d*h*rng.uniform(.035,.10);line(root,base,.015,stemmat)
   if arch=='cane':end[2]*=.60
   ps=curve(base,end,h*.35 if arch=='cane' else 0,.008 if arch=='dwarf' else p.get('stem_radius',.026),stemmat)
   for t in (.32,.54,.73,.88):
    at=ps[int(t*8)]
    for sign in (-1,1):
     dest=at+d*spread*.26+np.array([-d[1],d[0],.4])*sign*spread*.25+[0,0,h*.15]
     daughter=shoot(at,dest,arch=='compound')
     if arch not in ('broom',) and (arch!='dwarf' or p.get('fine_forks')):
      for k in range(p.get('fork_twigs',2)):
       fork=daughter[min(7,4+k*2)]
       offset=np.array([rng.uniform(-1,1),rng.uniform(-1,1),rng.uniform(.2,.8)])*h*.16
       shoot(fork,dest+offset,arch=='compound')
   shoot(ps[5],end+[0,0,h*.04],arch=='compound')
   if arch=='broom':
    for j in range(4):curve(ps[3],end+d*spread*.25+[0,0,h*rng.uniform(-.1,.15)],h*.02,.009)
   if r['profile'] in SPRING_FLOWERS and rng.random()<.65:bloom(end,1.2)
 elif arch in ('scoparius','ulex'):
  woody=True
  for i in range(int(rng.integers(9,15))):
   theta=i*2.399+rng.uniform(-.3,.3);d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*h*.07;line(root,base,.012,stemmat)
   end=d*spread*rng.uniform(.5,1.)+[0,0,h*rng.uniform(.65,1.)];ps=curve(base,end,h*.05,.008,stemmat)
   for j in range(2,8):
    for sign in (-1,1):
     side=np.array([-d[1],d[0],0])*sign;dest=ps[j]+d*h*.09+side*h*rng.uniform(.07,.18)+[0,0,h*rng.uniform(.12,.25)];twig=curve(ps[j],dest,h*.03,.003)
     for k in range(2,9):
      at=twig[k]
      if arch=='ulex':
       for q in (-1,1):line(at,at+side*q*.025+d*.02+[0,0,.02],.002)
       leafcount+=2  # Mature Ulex foliage is modified into these spines.
      elif k<5:
       for q in (-1,0,1):leaf(at,side+ d*q*.65+[0,0,.3],.015,.005)
      if rng.random()<(.24 if arch=='ulex' else .42):
       flower=at+side*.012;line(at,flower,.002);line(flower,flower+[0,0,.018],.008,head)
 elif arch in ('ponticum','kalmia'):
  woody=True
  for i in range(int(rng.integers(5,9))):
   theta=i*2.399+rng.uniform(-.3,.3);d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*h*.07;line(root,base,.02,stemmat)
   end=d*spread*rng.uniform(.5,1.)+[0,0,h*rng.uniform(.65,1.)];ps=curve(base,end,h*.07,.025,stemmat)
   for j in (2,4,6,8):
    for sign in (-1,1):
     side=np.array([-d[1],d[0],0])*sign;dest=ps[j]+d*h*.12+side*h*rng.uniform(.08,.20)+[0,0,h*.10];branch=curve(ps[j],dest,h*.02,.005,stemmat)
     for q in range(1,9):
      terminal=branch[q]+side*h*rng.uniform(.03,.10)+[0,0,h*rng.uniform(.01,.07)];line(branch[q],terminal,.003,stemmat)
      for k in range(7):
       angle=k*2.399;ld=np.array([math.cos(angle),math.sin(angle),rng.uniform(-.45,.35)]);leaf(terminal,ld,.14 if arch=='ponticum' else .085,.026 if arch=='ponticum' else .02)
      if q>=4 and rng.random()<.45:
       center=terminal+[0,0,.07 if arch=='ponticum' else .045];line(terminal,center,.003)
       for k in range(9):
        angle=k*2.399;radius=(.055 if arch=='ponticum' else .04)*math.sqrt((k+1)/9);tip=center+[math.cos(angle)*radius,math.sin(angle)*radius,rng.uniform(0,.025)];line(center,tip,.002)
        if arch=='ponticum':
         for lobe in range(5):
          a=lobe*math.tau/5;line(tip,tip+[math.cos(a)*.02,math.sin(a)*.02,.006],.006,head)
        else:line(tip,tip+[0,0,.01],.012,head)
 elif arch=='viburnum':
  woody=True
  for i in range(int(rng.integers(5,9))):
   theta=i*2.399+rng.uniform(-.4,.4);d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*h*.06
   line(root,base,.018,stemmat);end=d*spread*rng.uniform(.4,1.)+[0,0,h*rng.uniform(.7,1.)];ps=curve(base,end,h*.07,.018,stemmat)
   for j in (1,3,5,7):
    at=ps[j];dest=at+d*spread*rng.uniform(.18,.4)+np.array([-d[1],d[0],0])*spread*rng.uniform(-.2,.2)+[0,0,h*rng.uniform(.04,.16)];branch=curve(at,dest,h*.015,.004,stemmat)
    for k in (2,4,6,8):
     for sign in (-1,1):
      direction=np.array([-d[1]*sign,d[0]*sign,.25]);side=np.array([d[0],d[1],0]);petiole=branch[k]+direction*.015;line(branch[k],petiole,.002)
      for lobe in (-1,0,1):leaf(petiole,direction+side*lobe*.65,.075 if lobe==0 else .055,.025)
      forkend=branch[k]+direction*h*rng.uniform(.08,.16)+[0,0,h*rng.uniform(-.01,.07)];twig=curve(branch[k],forkend,h*.01,.003,stemmat)
      for q in (2,4,6,8):
       for leafsign in (-1,1):
        ld=side*leafsign+[0,0,.2]
        for lobe in (-1,0,1):leaf(twig[q],ld+direction*lobe*.65,.075 if lobe==0 else .055,.025)
    if j>=3 and rng.random()<.6:
     center=dest+[0,0,.045];line(dest,center,.003)
     for k in range(10):
      angle=k*math.tau/10;edge=center+[math.cos(angle)*.06,math.sin(angle)*.06,0];line(center,edge,.002);line(edge,edge+[0,0,.010],.013,head)
     for k in range(7):
      angle=k*2.399;line(center,center+[math.cos(angle)*.03,math.sin(angle)*.03,.012],.004,head)
 elif arch=='fern':
  for i in range(int(rng.integers(p.get('fronds_min',6),p.get('fronds_max',9)))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);length=h*rng.uniform(.75,1.05)
   openness=rng.uniform(p.get('frond_open_min',.32),p.get('frond_open_max',.78))
   ps=curve(root,d*length*openness+[0,0,length*(1.0-openness*.65)],length*.18)
   pairs=p.get('pinna_pairs',7);frond_start=p.get('bare_stipe',0.)
   for j in range(1,pairs+1):
    t=frond_start+(1-frond_start)*j/(pairs+1);idx=min(7,int(t*8));at=ps[idx]+(ps[idx+1]-ps[idx])*(t*8-idx)
    tangent=unit(ps[min(8,idx+1)]-ps[max(0,idx-1)])
    w=length*p.get('frond_width',.18)*((1-t) if p.get('triangular_frond') else math.sin(math.pi*t)**.8)
    for sign in (-1,1):
     side=np.array([-d[1],d[0],.1])*sign
     end=at+side*w+d*length*.07;line(at,end)
     if p.get('simple_pinnae'):leaf(at,end-at,np.linalg.norm(end-at),w*.12)
     else:
      for k in range(1,5):
       for s in (-1,1):leaf(at+(end-at)*k/5,side*.35+tangent*s,w*.30,w*.085)
 elif arch=='cattail':
  # Typha has basal strap leaves and separate unbranched culms, not reed nodes.
  for i in range(int(rng.integers(12,19))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=d*spread*rng.uniform(.05,.22);base[2]=.015;line(root,base)
   length=h*rng.uniform(.60,1.);end=base+d*spread*rng.uniform(.25,.75)+[0,0,length]
   ps=curve(base,end,length*.08,.004)
   for a,b in zip(ps[:-1],ps[1:]):leaf(a,b-a,np.linalg.norm(b-a)*1.15,.013)
  for i in range(2):
   theta=rng.uniform(0,math.tau);base=np.array([math.cos(theta)*spread*.15,math.sin(theta)*spread*.15,.015]);line(root,base)
   top=base+[h*rng.uniform(-.05,.05),0,h*rng.uniform(.75,.96)]
   line(base,top,.008,wood);line(top-[0,0,min(.18,h*.12)],top,.022,wood)
 elif arch=='woodrush':
  # Evergreen woodland tufts: long basal blades bend back toward the floor.
  for i in range(int(rng.integers(16,23))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=d*spread*rng.uniform(0,.12);base[2]=.015;line(root,base,.003)
   length=h*rng.uniform(.65,1.);end=base+d*spread*rng.uniform(.8,1.6)+[0,0,length*rng.uniform(.12,.4)]
   ps=curve(base,end,length*.55,.003)
   for a,b in zip(ps[:-1],ps[1:]):leaf(a,b-a,np.linalg.norm(b-a)*1.15,.009)
 elif arch in ('dactylis','phleum','glyceria','eriophorum'):
  # Spring foliage: arching basal blades plus separate leafy upright culms.
  for i in range(p.get('basal_blades',24)):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.05,.65);line(root,base,.002)
   length=h*rng.uniform(.35,.75);end=base+d*length*rng.uniform(.3,.7)+[0,0,length*rng.uniform(.15,.45)]
   ps=curve(base,end,length*.38,.002)
   for a,b in zip(ps[:-1],ps[1:]):leaf(a,b-a,np.linalg.norm(b-a)*1.1,p.get('blade_halfwidth',.004))
  for i in range(p.get('culms',7)):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.1,.8);line(root,base,.002)
   tip=base+d*h*rng.uniform(.02,.14)+[0,0,h*rng.uniform(.65,1.)];ps=curve(base,tip,h*.025,.003)
   for j in (2,4,6):
    at=ps[j];side=d*(-1 if j==4 else 1);length=min(p.get('max_blade',.35),h*.45)*(1-j*.065)
    ls=curve(at,at+side*length*.8+[0,0,-length*.18],length*.2,.002)
    for a,b in zip(ls[:-1],ls[1:]):leaf(a,b-a,np.linalg.norm(b-a)*1.1,p.get('blade_halfwidth',.004))
 elif arch=='juncus':
  # Cylindrical leafless green stems form a dense tuft, never a broad-leaf fan.
  for i in range(42):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);rad=spread*rng.uniform(.05,.65);base=root+d*rad;line(root,base,.002)
   tip=base+d*h*rng.uniform(.04,.22)+[0,0,h*rng.uniform(.5,1.)];curve(base,tip,h*.01,.0025)
 elif arch in ('fluviatile','sylvaticum'):
  for i in range(3 if arch=='sylvaticum' else 9):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,1.);line(root,base,.002)
   tip=base+d*h*.04+[0,0,h*rng.uniform(.75,1.)];line(base,tip,.003)
   for j in ((2,4,6) if arch=='sylvaticum' else range(2,8)):
    if arch=='fluviatile' and (i%3==0 or rng.random()<.45):continue
    at=base+(tip-base)*j/8
    for k in range(4 if arch=='sylvaticum' else 6):
     angle=k*math.tau/(4 if arch=='sylvaticum' else 6)+j*.16;out=np.array([math.cos(angle),math.sin(angle),0]);length=h*(.42 if arch=='sylvaticum' else .10)*(1-j*.06)
     end=at+out*length+[0,0,-length*.18 if arch=='sylvaticum' else length*.35];line(at,end,.0015)
     if arch=='sylvaticum':
      side=np.array([-out[1],out[0],0])
      for q in (4,):
       point=at+(end-at)*q/8
       for sign in (-1,1):line(point,point+out*length*.22+side*sign*length*.28+[0,0,-length*.16],.001)
 elif arch in ('grass','sedge','strap','rush','reed','cotton','burreed'):
  for i in range(int(rng.integers(p.get('blade_count_min',9),p.get('blade_count_max',16)))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=d*spread*rng.uniform(0,.25)
   base[2]=.015;line(root,base)
   length=h*rng.uniform(.55,1.0);end=base+d*spread*rng.uniform(.65,1.35)+[0,0,length*(.85 if arch in ('grass','sedge','strap') else 1)]
   ps=curve(base,end,length*.15,.003 if arch in ('grass','sedge') else .01)
   if arch not in ('rush','cotton'): 
    if arch in ('reed','cattail','burreed'):
     for j in (2,4,6):leaf(ps[j],d*.8+[0,0,.5],h*.35,h*.018)
    else:
     for a,b in zip(ps[:-1],ps[1:]):leaf(a,b-a,np.linalg.norm(b-a)*1.2,h*p.get('blade_width_fraction',.025 if arch=='strap' else .006))
   # Seed heads are deliberately absent in spring except diagnostic residual
   # cattail heads; these are old brown heads, not fresh summer inflorescences.
   if arch=='cattail' and i%5==0:line(ps[-2],end,.025,wood)
 elif arch=='carexsylvatica':
  for i in range(30):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.02,.3);line(root,base,.002)
   length=h*rng.uniform(.6,1.);ps=curve(base,base+d*length*.75+[0,0,length*.15],length*.4,.002)
   for a,b in zip(ps[:-1],ps[1:]):leaf(a,b-a,np.linalg.norm(b-a)*1.1,.004)
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*.15;ps=curve(base,base+d*h*.5+[0,0,h*.75],h*.3,.002)
   for j in (4,6,8):
    at=ps[j];end=at+d*.03+[0,0,-.04];line(at,end,.002);line(end,end+[0,0,-.035],.004)
 elif arch in ('rivulare','lanuginosum'):
  for i in range(65):
   theta=rng.uniform(0,math.tau);d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*math.sqrt(rng.random());line(root,base,.002)
   end=base+d*h*(.8 if arch=='rivulare' else .15)+[0,0,h*rng.uniform(.35,.9)];ps=curve(base,end,h*.08,.002)
   for j in (2,4,6):
    angle=theta+j;side=np.array([math.cos(angle),math.sin(angle),0]);at=ps[j]
    for sign in (-1,1):
     tip=at+side*sign*h*.25+[0,0,h*.1];line(at,tip,.002)
     for k in range(3):leaf(at+(tip-at)*k/3,side*sign+[0,0,.5],.005,.0015)
 elif arch=='peatmoss':
  for i in range(130):
   theta=rng.uniform(0,math.tau);rad=spread*math.sqrt(rng.random());base=root+[math.cos(theta)*rad,math.sin(theta)*rad,0];line(root,base,.002)
   tip=base+[0,0,h*(.55+.35*(1-rad/max(spread,.001)))*rng.uniform(.85,1.1)];line(base,tip,.002)
   for k in range(5):
    angle=k*math.tau/5+theta;outward=np.array([math.cos(angle)*.012,math.sin(angle)*.012,0]);line(tip,tip+outward,.003)
    at=base+(tip-base)*.7;line(at,at+outward+[0,0,-.02],.002)
 elif arch=='haircap':
  for i in range(55):
   theta=rng.uniform(0,math.tau);d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*math.sqrt(rng.random());line(root,base,.002)
   tip=base+d*h*.05+[0,0,h*rng.uniform(.55,1.)];line(base,tip,.002)
   for j in range(3,8):
    at=base+(tip-base)*j/8
    for k in range(6):
     angle=k*math.tau/6+j;line(at,at+[math.cos(angle)*.007,math.sin(angle)*.007,.003],.001)
 elif arch=='feathermoss':
  # Pleurocarpous moss makes interwoven reclining pinnate shoots, not a tuft.
  for i in range(55):
   theta=rng.uniform(0,math.tau);d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*math.sqrt(rng.random());line(root,base,.002)
   direction=rng.uniform(0,math.tau);forward=np.array([math.cos(direction),math.sin(direction),0]);side=np.array([-forward[1],forward[0],0]);end=base+forward*h*rng.uniform(.7,1.4)+[0,0,h*rng.uniform(.2,.65)];ps=curve(base,end,h*.10,.002)
   for j in range(2,8):
    for sign in (-1,1):line(ps[j],ps[j]+side*sign*h*.25+forward*h*.08,.002)
 elif arch in ('moss','mat'):
  # Low shoots grow from an irregular connected rhizoid lattice, not a sphere.
  for i in range(75):
   theta=rng.uniform(0,math.tau);rad=spread*1.8*math.sqrt(rng.random());base=np.array([math.cos(theta)*rad,math.sin(theta)*rad,.015]);line(root,base,.009)
   end=base+[0,0,h*rng.uniform(.25,.9)*(1-.35*rad/max(.01,spread*1.8))];line(base,end)
   if arch=='moss':
    for j in range(1,5):
     at=base+(end-base)*j/5;leaf(at,[math.cos(theta+j),math.sin(theta+j),.4],h*.16,h*.035)
 elif arch in ('submerged','horsetail'):
  for i in range(7):
   theta=i*2.399;end=np.array([math.cos(theta)*spread,math.sin(theta)*spread,h*rng.uniform(.6,1)]);ps=curve(root,end,h*.08)
   for j in range(1,8):
    for k in range(6):
     angle=k*math.tau/6+j*.2;d=np.array([math.cos(angle),math.sin(angle),.25]);at=ps[j];length=h*.15*(1-j*.08)
     if arch=='horsetail':line(at,at+d*length,.007)
     else:leaf(at,d,length,length*.12)
 elif arch in ('asclepias','chamaenerion','impatiens','lythrum','mentha'):
  for i in range(4):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.7);line(root,base,.003)
   tip=base+d*h*.12+[0,0,h*rng.uniform(.65,1.)];ps=curve(base,tip,0,.004)
   for j in range(2,8):
    angle=theta+j*2.399;out=np.array([math.cos(angle),math.sin(angle),.25]);length=min(p['leaf_m'],h*.4)*rng.uniform(.7,1.)
    for sign in ((-1,1) if arch in ('asclepias','lythrum','mentha') else (1,)):
     leaf(ps[j],out*[sign,sign,1],length,min(p['leaf_width_m'],length*.25))
   if arch=='impatiens':
    for j in (3,5):
     end=ps[j]-d*h*.18+[0,0,h*.2];branch=curve(ps[j],end,0,.003)
     for k in (3,6):leaf(branch[k],-d+[0,0,.3],min(.08,h*.3),.02)
 elif arch in ('centaurea','knautia','campanula'):
  for i in range(4):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.15,.6);line(root,base,.002)
   for k in range(6):
    angle=k*2.399;out=np.array([math.cos(angle),math.sin(angle),.25])
    if arch=='campanula':
     at=base+out*.025;line(base,at,.002);leaf(at,out,min(.03,h*.3),.013)
    else:leaf(base,out,h*.45,h*.055)
   tip=base+d*h*.12+[0,0,h*rng.uniform(.6,1.)];ps=curve(base,tip,0,.002)
   for j in (2,4,6):
    out=d*(1 if j%4 else -1)+[0,0,.25];length=h*(.35 if arch!='campanula' else .22)
    if arch=='knautia':
     end=ps[j]+unit(out)*length;line(ps[j],end,.002);side=np.array([-d[1],d[0],0])
     for q in (1,2,3):
      for sign in (-1,1):leaf(ps[j]+(end-ps[j])*q/4,side*sign+out*.3,length*.3,length*.035)
    else:leaf(ps[j],out,length,.003 if arch=='campanula' else length*.12)
 elif arch=='papaver':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.7);line(root,base,.002)
   tip=base+d*h*.1+[0,0,h*rng.uniform(.7,1.)];ps=curve(base,tip,0,.002)
   for j in (2,4):
    out=d*(1 if j==2 else -1);end=ps[j]+out*min(.15,h*.3);line(ps[j],end,.002)
    for q in range(1,5):
     for sign in (-1,1):leaf(ps[j]+(end-ps[j])*q/5,out*.5+np.array([-d[1],d[0],.1])*sign,min(.04,h*.1),.007)
   if i%3==0:
    for k in range(12):
     angle=k*math.tau/12;line(tip,tip+[math.cos(angle)*.025,math.sin(angle)*.025,.01],.006,head)
    line(tip,tip+[0,0,.005],.006,M.MAT_BARK)
   else:
    end=tip+d*.025+[0,0,-.02];curve(tip,end,.015,.002);line(end,end+[0,0,-.015],.005)
 elif arch=='comarum':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.4,1.);curve(root,base,.025,.003,wood)
   tip=base+d*h*.2+[0,0,h*rng.uniform(.5,1.)];ps=curve(base,tip,0,.003)
   for j in (3,6):
    out=d*(1 if j==3 else -1);end=ps[j]+out*.10;line(ps[j],end,.002);side=np.array([-d[1],d[0],0])
    for q in (1,2):
     for sign in (-1,1):leaf(ps[j]+(end-ps[j])*q/3,out*.4+side*sign,.045,.008)
    leaf(end,out,.05,.01)
 elif arch=='trapa':
  for i in range(3):
   angle=i*2.399;base=root+np.array([math.cos(angle),math.sin(angle),0])*rng.uniform(.1,.3)*h/.05;line(root,base,.002)
   radius=rng.uniform(.08,.16)*(.7+.3*h/.05)
   for k in range(9):
    theta=k*math.tau/9+rng.uniform(-.15,.15);d=np.array([math.cos(theta),math.sin(theta),0]);at=base+d*radius+[0,0,.025];line(base,at,.002)
    leaf(at,d,.04,.017)
 elif arch=='callitriche':
  for i in range(12):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);end=root+d*rng.uniform(.07,.25)*h/.05;ps=curve(root,end,0,.001)
   for j in (3,5,8):
    for k in range(6):
     angle=theta+k*math.tau/6;leaf(ps[j],[math.cos(angle),math.sin(angle),0],rng.uniform(.008,.018),.002)
 elif arch=='nymphoides':
  for i in range(7):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);center=root+d*rng.uniform(.08,.32)*h/.05+[0,0,.025];line(root,center,.002)
   radius=rng.uniform(.035,.055)
   for angle in np.linspace(-2.5,2.5,27):
    out=np.array([math.cos(theta+angle),math.sin(theta+angle),0]);line(center,center+out*radius,.005)
 elif arch=='potamogeton':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.7);line(root,base,.002)
   tip=base+d*h*.35+[0,0,h*rng.uniform(.6,1.)];ps=curve(base,tip,0,.002)
   for j in range(1,9):
    angle=theta+j*2.4;out=np.array([math.cos(angle),math.sin(angle),.2]);length=rng.uniform(.04,.06);leaf(ps[j],out,length,.006)
 elif arch in ('sagittaria','alisma','pontederia'):
  for i in range(9):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*.15;line(root,base,.003)
   at=base+d*spread*rng.uniform(.3,.8)+[0,0,h*rng.uniform(.4,.65)];curve(base,at,0,.003)
   length=h*rng.uniform(.28,.42);out=d*.5+[0,0,.8]
   leaf(at,out,length,length*(.28 if arch=='alisma' else .22))
   if arch in ('sagittaria','pontederia'):
    for sign in (-1,1):
     side=np.array([-d[1],d[0],0]);leaf(at,-unit(out)*.4+side*sign,length*(.55 if arch=='sagittaria' else .25),length*.10)
 elif arch in ('sparganium','butomus','acorus','pseudacorus'):
  for i in range(4):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*.35;line(root,base,.003)
   for k in range(7):
    sign=-1 if k%2 else 1;length=h*rng.uniform(.55,1.);out=d*sign
    end=base+out*length*rng.uniform(.15,.5)+[0,0,length*rng.uniform(.6,.9)];ps=curve(base,end,length*.12,.003)
    side=np.array([-d[1],d[0],0])
    for j in range(1,8):
     width=p['blade_halfwidth']*math.sin(j*math.pi/9);line(ps[j]-side*width,ps[j]+side*width,.003)
   if arch=='pseudacorus' and i%2==0:
    tip=base+d*h*.12+[0,0,h*rng.uniform(.8,1.)];line(base,tip,.003)
    for k in range(3):
     angle=k*math.tau/3+theta;out=np.array([math.cos(angle),math.sin(angle),0]);bend=tip+out*.035;line(tip,bend,.009,head);line(bend,bend+[0,0,-.035],.010,head);line(tip,tip+out*.012+[0,0,.025],.006,head)
   if arch=='acorus' and i==0:
    at=base+d*h*.08+[0,0,h*.5];line(base,at,.003);line(at,at+d*.025+[0,0,.07],.006)
 elif arch in ('schoenoplectus','eleocharis'):
  for i in range(14 if arch=='eleocharis' else 18):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.9);line(root,base,.001)
   end=base+d*h*rng.uniform(.01,.12)+[0,0,h*rng.uniform(.55,1.)];curve(base,end,0,.0003 if arch=='eleocharis' else .005)
 elif arch=='acutiformis':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.7);line(root,base,.002)
   for k in range(7):
    angle=theta+k*2.399;out=np.array([math.cos(angle),math.sin(angle),0]);length=h*rng.uniform(.5,1.);end=base+out*length*.35+[0,0,length*.7];curve(base,end,length*.12,.003)
 elif arch in ('phragmites','zizania'):
  for i in range(7):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.8);line(root,base,.003)
   tip=base+d*h*.08+[0,0,h*rng.uniform(.65,1.)];ps=curve(base,tip,0,.004)
   for j in range(2,8):
    out=d*(1 if j%2 else -1);length=min(p['leaf_m'],h*.4)*rng.uniform(.65,1.);end=ps[j]+out*length+[0,0,-length*.2];blade=curve(ps[j],end,length*.25,.003);side=np.array([-d[1],d[0],0])
    for k in range(1,8):
     width=p['leaf_halfwidth']*math.sin(k*math.pi/9);line(blade[k]-side*width,blade[k]+side*width,.003)
 elif arch=='hedera':
  for i in range(7):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);end=root+d*spread*rng.uniform(.7,1.5)+[0,0,h*.3];ps=curve(root,end,h*.05,.003,wood)
   for j in (2,4,6,8):
    side=np.array([-d[1],d[0],0])*(1 if j%4 else -1);at=ps[j]+side*.04+[0,0,h*.2];line(ps[j],at,.002)
    for angle in (-.7,0,.7):
     out=d*math.cos(angle)+side*math.sin(angle);leaf(at,out,.07 if angle==0 else .045,.016)
 elif arch=='fontinalis':
  for i in range(16):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.1,.5);line(root,base,.001)
   end=base+d*h*rng.uniform(1.,2.)+[0,0,h*rng.uniform(.2,.6)];ps=curve(base,end,h*.1,.001)
   for j in (2,4,6):
    side=np.array([-d[1],d[0],0]);fork=ps[j]+d*h*.4+side*h*.3*(1 if j%4 else -1);qs=curve(ps[j],fork,0,.001)
    for k in range(1,9):leaf(qs[k],side+[0,0,.2],.005,.002)
 elif arch=='myosotis':
  for i in range(6):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.3,.7);line(root,base,.002)
   tip=base+d*h*.3+[0,0,h*rng.uniform(.6,1.)];ps=curve(base,tip,0,.002)
   for j in range(2,8):leaf(ps[j],d*(1 if j%2 else -1)+[0,0,.3],min(.08,h*.3),.007)
 elif arch=='nasturtium':
  for i in range(6):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.8);line(root,base,.002)
   tip=base+d*h*.3+[0,0,h*rng.uniform(.4,.8)];ps=curve(base,tip,h*.05,.003)
   for j in (2,4,6):
    out=d*(1 if j%4 else -1);end=ps[j]+out*.10;line(ps[j],end,.002);side=np.array([-d[1],d[0],0])
    for q in (1,2):
     for sign in (-1,1):leaf(ps[j]+(end-ps[j])*q/3,side*sign+out*.2,.025,.01)
    leaf(end,out,.04,.016)
   if i%3==0:
    at=tip+[0,0,.035];line(tip,at,.002)
    for k in range(4):
     angle=k*math.tau/4;end=at+[math.cos(angle)*.018,math.sin(angle)*.018,.006];line(at,end,.001);line(end,end+[0,0,.004],.004,head)
 elif arch in ('elodea','ceratophyllum','myriophyllum'):
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.7);line(root,base,.001)
   end=base+d*h*.6+[0,0,h*rng.uniform(.6,1.)];ps=curve(base,end,h*.03,.001)
   for j in range(1,9):
    for k in range(3 if arch=='elodea' else 4):
     angle=theta+k*math.tau/(3 if arch=='elodea' else 4)+j*.2;out=np.array([math.cos(angle),math.sin(angle),.15]);side=np.array([-out[1],out[0],0])
     if arch=='elodea':leaf(ps[j],out,.011,.002)
     elif arch=='ceratophyllum':
      at=ps[j]+out*.013;line(ps[j],at,.001)
      for sign in (-1,1):line(at,at+out*.012+side*.008*sign,.001)
     else:
      at=ps[j]+out*.04;line(ps[j],at,.001)
      for q in range(1,5):
       for sign in (-1,1):line(ps[j]+out*.04*q/5,ps[j]+out*(.04*q/5+.005)+side*.01*sign,.001)
 elif arch in ('isoetes','stratiotes'):
  for i in range(8 if arch=='isoetes' else 14):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);length=h*rng.uniform(.6,1.);end=root+d*length*(rng.uniform(.25,.65) if arch=='isoetes' else .85)+[0,0,length*.75];ps=curve(root,end,length*.08,.0015 if arch=='isoetes' else .003)
   if arch=='stratiotes':
    side=np.array([-d[1],d[0],0])
    for j in range(1,8):line(ps[j]-side*.009*math.sin(j*math.pi/9),ps[j]+side*.009*math.sin(j*math.pi/9),.003)
 elif arch=='nymphaea':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);center=root+d*rng.uniform(.15,.55)*h/.05+[0,0,.025];line(root,center,.002)
   radius=rng.uniform(.08,.15)
   for angle in np.linspace(-2.6,2.6,45):
    out=np.array([math.cos(theta+angle),math.sin(theta+angle),0]);line(center,center+out*radius,.005)
 elif arch=='nuphar':
  # Waterline patch: petiole depth is a placement concern, not above-water height.
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);side=np.array([-d[1],d[0],0]);at=root+d*rng.uniform(.15,.45)*h/.05;line(root,at,.003)
   radius=rng.uniform(.075,.15)*(.7+.3*h/.05);center=at+[0,0,.025]
   line(at,center,.003)
   for angle in np.linspace(-2.55,2.55,45):line(center,center+d*math.cos(angle)*radius+side*math.sin(angle)*radius*.8,.006)
 elif arch=='salvinia':
  # Subvoxel paired fronds remain a colony-scale surface chain, never giant pads.
  for i in range(14):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);side=np.array([-d[1],d[0],0]);base=root+d*rng.uniform(.02,.16)*h/.05;line(root,base,.001)
   ps=curve(base,base+d*rng.uniform(.05,.12),.004,.001)
   for j in (1,3,5,7):
    for sign in (-1,1):leaf(ps[j],side*sign,.012,.003)
 elif arch=='cladophora':
  for i in range(32):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.1,1.);line(root,base,.001)
   end=base+d*h*rng.uniform(.4,1.5)+[0,0,h*rng.uniform(.15,.7)];ps=curve(base,end,h*.05,.001)
   for j in (2,4,6):
    side=np.array([-d[1],d[0],0]);at=ps[j]
    for sign in (-1,1):line(at,at+d*h*.3+side*sign*h*.2+[0,0,h*.12],.001)
 elif arch=='chara':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,1);line(root,base,.001)
   tip=base+d*h*.1+[0,0,h*rng.uniform(.65,1.)];line(base,tip,.001)
   for j in (2,4,6):
    at=base+(tip-base)*j/8
    for k in range(5):
     angle=k*math.tau/5+j*.2;line(at,at+[math.cos(angle)*h*.13,math.sin(angle)*h*.13,h*.035],.001)
 elif arch=='fluitans':
  # A trailing submerged segment; authored length must not become vertical height.
  length=p['trailing_length_m']*h/p['baseline_height_m'];flow=np.array([1.,.1,0])
  for i in range(4):
   base=root+[rng.uniform(0,.1),(i-1.5)*.08+rng.uniform(-.03,.03),0];line(root,base,.001);end=base+flow*length*rng.uniform(.65,1.)+[0,rng.uniform(-.08,.08),h*.3];ps=curve(base,end,h*.2,.001)
   for j in (2,4,6):
    at=ps[j]
    for sign in (-1,1):
     tip=at+flow*rng.uniform(.10,.20)+[0,sign*rng.uniform(.025,.06),.01];line(at,tip,.001);line(tip,tip+flow*.06+[0,sign*.018,0],.001)
 elif arch=='carexcespitosa':
  for i in range(25):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.02,.2);line(root,base,.001)
   length=h*rng.uniform(.4,.8);ps=curve(base,base+d*length*.7+[0,0,length*.2],length*.35,.001)
   for a,b in zip(ps[:-1],ps[1:]):leaf(a,b-a,np.linalg.norm(b-a),.0015)
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);tip=root+d*h*.12+[0,0,h*rng.uniform(.8,1.)];line(root,tip,.002);line(tip-[0,0,.02],tip,.004,wood)
 elif arch in ('floating','runner'):
  for i in range(9):
   angle=i*2.399;d=np.array([math.cos(angle),math.sin(angle),0]);base=root+d*spread*2*rng.uniform(.3,1);line(root,base)
   leaf(base,d+[0,0,.1],h*.5,h*.22)
 elif arch in ('lotus','trifolium'):
  for i in range(6):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.8);line(root,base,.002)
   tip=base+d*h*.3+[0,0,h*rng.uniform(.6,1)];ps=curve(base,tip,h*.04,.002)
   for j in (2,4,6):
    at=ps[j];pet=at+d*.035;line(at,pet,.002)
    for k in range(3):
     angle=theta+k*math.tau/3;leaf(pet,[math.cos(angle),math.sin(angle),.1],.04 if arch=='trifolium' else .025,.014 if arch=='trifolium' else .007)
   if arch=='trifolium':line(tip,tip+[0,0,.025],.014,head)
   else:
    for k in range(3):
     angle=theta+k*math.tau/3;end=tip+[math.cos(angle)*.012,math.sin(angle)*.012,0];line(tip,end,.002);line(end,end+[0,0,.012],.007,head)
 elif arch in ('bellis','leucanthemum'):
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.25,1);line(root,base,.002)
   for k in range(7):
    angle=k*2.399+theta;leaf(base,[math.cos(angle),math.sin(angle),.15],min(.12,h*.5),min(.025,h*.12))
   tip=base+d*h*.07+[0,0,h*rng.uniform(.7,1)];line(base,tip,.002)
   if arch=='leucanthemum':
    for j in (2,4,6):leaf(base+(tip-base)*j/9,d*((-1)**j)+[0,0,.3],.08*(1-j*.09),.012)
   radius=.018 if arch=='bellis' else .03
   for k in range(12):
    angle=k*math.tau/12;line(tip,tip+[math.cos(angle)*radius,math.sin(angle)*radius,0],.004,M.MAT_PLUME_WHITE)
   line(tip+[0,0,.007],tip+[0,0,.009],.009,M.MAT_SKIN_YELLOW)
 elif arch=='plantago':
  for i in range(4):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,1);line(root,base,.002)
   for k in range(9):
    angle=k*2.399;leaf(base,[math.cos(angle),math.sin(angle),rng.uniform(.2,.8)],h*.6,.018)
   tip=base+d*h*.12+[0,0,h*rng.uniform(.7,1.)];line(base,tip,.002);line(tip-[0,0,.025],tip,.008,wood)
 elif arch=='hederifolium':
  for i in range(10):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.1,.7);line(root,base,.002)
   at=base+d*h*.35+[0,0,h*rng.uniform(.3,.7)];curve(base,at,h*.05,.002)
   for k in (-1,0,1):leaf(at,d+[0,0,.05]+np.array([-d[1],d[0],0])*k*.65,.06 if k==0 else .045,.022)
 elif arch=='lupinus':
  for i in range(3):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.25,1);line(root,base,.002)
   top=base+[0,0,h*rng.uniform(.7,1.)];line(base,top,.003)
   for j in (1,2,3,4):
    at=base+(top-base)*j/7;angle=theta+j*2.4;end=at+[math.cos(angle)*h*.18,math.sin(angle)*h*.18,0];line(at,end,.002)
    for k in range(7):
     a=k*math.tau/7;leaf(end,[math.cos(a),math.sin(a),.1],h*.13,.012)
   for j in range(9):
    at=base+(top-base)*(.58+j*.045);angle=j*2.399;end=at+[math.cos(angle)*.022,math.sin(angle)*.022,0];line(at,end,.002);line(end,end+[0,0,.012],.008,head)
 elif arch=='achillea':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,1);line(root,base,.002)
   tip=base+d*h*.1+[0,0,h*rng.uniform(.65,1.)];line(base,tip,.002)
   for j in (1,3,5,7):
    at=base+(tip-base)*j/9;angle=theta+j*2.399;forward=np.array([math.cos(angle),math.sin(angle),.2]);end=at+forward*h*.3;line(at,end,.001)
    side=np.array([-forward[1],forward[0],0])
    for k in range(1,8):
     node=at+(end-at)*k/8
     for sign in (-1,1):line(node,node+side*sign*h*.055*(1-k*.09)+forward*.01,.001)
 elif arch=='erythronium':
  for i in range(int(rng.integers(3,6))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,1);line(root,base,.002)
   for sign in (-1,1):leaf(base,d*sign+[0,0,.18],h*.65,h*.10)
   if i==0 or rng.random()<.4:
    tip=base+d*h*.2+[0,0,h*.85];curve(base,tip,h*.22,.002)
    for k in range(6):
     angle=k*math.tau/6;petal=tip+[math.cos(angle)*.02,math.sin(angle)*.02,.025];line(tip,petal,.005,head)
    line(tip,tip+[0,0,-.02],.003,head)
 elif arch=='digitalis':
  for i in range(int(rng.integers(2,5))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*.6;line(root,base,.003)
   for k in range(8):
    a=k*2.399;leaf(base,[math.cos(a),math.sin(a),.2],min(.28,h*.32),min(.06,h*.07))
   tip=base+d*h*.09+[0,0,h*rng.uniform(.85,1.)];ps=curve(base,tip,h*.02,.004)
   for j in (1,2,3):leaf(ps[j],d+[0,0,.3],h*.16*(1-j*.17),h*.025)
   for k in range(15):
    t=.45+k*.033;at=base+(tip-base)*t;side=np.array([-d[1],d[0],0])*(-1 if k%2 else 1);end=at+d*.03+side*.02;line(at,end,.002);line(end,end+[0,0,-.03*(1-k*.025)],.010,head)
 elif arch=='helleborus':
  for i in range(int(rng.integers(6,10))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);tip=root+d*spread*.7+[0,0,h*rng.uniform(.4,.7)];line(root,tip,.003)
   for k in range(7):
    angle=theta+(k-3)*.43;leaf(tip,[math.cos(angle),math.sin(angle),-.12],h*rng.uniform(.22,.35),h*.055)
  for i in range(3):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);tip=root+d*spread*.6+[0,0,h*.85];curve(root,tip,h*.06,.003)
   for j in range(2):
    center=tip+d*.04*j+[0,0,-.035*j];line(tip,center,.002)
    for k in range(5):
     a=k*math.tau/5;line(center,center+[math.cos(a)*.022,math.sin(a)*.022,-.01],.008,head)
 elif arch=='menyanthes':
  for i in range(6):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.3,1.);line(root,base,.003);tip=base+[0,0,h*rng.uniform(.35,.6)];line(base,tip,.003)
   for k in range(3):
    a=theta+(k-1)*1.;leaf(tip,[math.cos(a),math.sin(a),.1],h*.25,h*.075)
   if i<3:
    top=base+[0,0,h];line(base,top,.003)
    for j in range(7):
     a=j*2.399;at=base+[0,0,h*(.6+j*.055)];end=at+[math.cos(a)*.025,math.sin(a)*.025,0];line(at,end,.002);line(end,end+[0,0,.008],.008,head)
 elif arch=='caltha':
  for i in range(8):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);tip=root+d*spread*rng.uniform(.5,1.)+[0,0,h*rng.uniform(.25,.65)];curve(root,tip,h*.05,.003)
   side=np.array([-d[1],d[0],0])
   for sign in (-1,1):leaf(tip,d+side*sign*.45+[0,0,.08],h*.22,h*.11)
   if i<5:
    end=tip+d*h*.12+[0,0,h*.22];line(tip,end,.003)
    for k in range(5):
     a=k*math.tau/5;line(end,end+[math.cos(a)*.018,math.sin(a)*.018,.008],.006,head)
 elif arch=='geranium':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);tip=root+d*spread+[0,0,h*rng.uniform(.7,1.)];ps=curve(root,tip,h*.03,.002)
   for j in (2,4,6):
    at=ps[j];end=at+np.array([-d[1],d[0],.2])*h*.14;line(at,end,.002)
    for k in range(5):
     a=theta+(k-2)*.6;ld=np.array([math.cos(a),math.sin(a),0]);axis=end+ld*h*.16;line(end,axis,.002)
     for q in (1,2,3):
      for sign in (-1,1):leaf(end+(axis-end)*q/3,ld*.4+np.array([-ld[1],ld[0],0])*sign,h*.055,h*.012)
   for sign in (-1,1):
    end=tip+np.array([-d[1],d[0],.5])*sign*.025;line(tip,end,.002)
    for k in range(5):
     a=k*math.tau/5;line(end,end+[math.cos(a)*.007,math.sin(a)*.007,0],.002,head)
 elif arch=='silene':
  for i in range(4):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*.5;line(root,base,.003);tip=base+d*h*.1+[0,0,h*rng.uniform(.8,1.)];ps=curve(base,tip,0,.003)
   for j in (1,3,5):
    for sign in (-1,1):leaf(ps[j],d*sign+[0,0,.2],h*.18*(1-j*.08),h*.035)
   for sign in (-1,1):
    end=ps[5]+np.array([-d[1],d[0],0])*h*.15*sign+[0,0,h*.25];line(ps[5],end,.002)
    for k in range(5):
     a=k*math.tau/5;line(end,end+[math.cos(a)*.012,math.sin(a)*.012,.003],.004,head)
 elif arch=='ajuga':
  for i in range(5):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*.7;line(root,base,.002)
   for k in range(6):
    a=k*2.399;leaf(base,[math.cos(a),math.sin(a),.12],h*.30,h*.07)
   top=base+[0,0,h*rng.uniform(.75,1.)];line(base,top,.003)
   for j in range(2,8):
    at=base+(top-base)*j/8
    for sign in (-1,1):
     leaf(at,d*sign+[0,0,.2],h*.12*(1-j*.07),h*.03)
     end=at+d*sign*.015;line(at,end,.002);line(end,end+[0,0,.008],.005,head)
 elif arch=='violet':
  for i in range(int(rng.integers(4,8))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,1.);line(root,base,.002)
   at=base+[0,0,h*.35];line(base,at,.002)
   for sign in (-1,1):leaf(at,d*sign+[0,0,.1],h*.30,h*.13)
   if i>0 and rng.random()<.65:continue
   tip=base+d*h*.12+[0,0,h*rng.uniform(.75,1.)];line(base,tip,.002)
   # Upright five-part corolla; its real 15-20 mm diameter is sub-voxel.
   side=np.array([-d[1],d[0],0])
   for k in range(5):
    angle=k*math.tau/5;line(tip,tip+side*math.cos(angle)*.008+[0,0,math.sin(angle)*.008],.0025,head)
 elif arch=='cowslip':
  for k in range(int(rng.integers(7,11))):
   angle=k*2.399;leaf(root,[math.cos(angle),math.sin(angle),.35],h*rng.uniform(.35,.52),h*.065)
  for i in range(int(rng.integers(2,5))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);tip=root+d*spread*.30+[0,0,h*rng.uniform(.75,1.)];line(root,tip,.003)
   for k in range(5):
    angle=theta+k*.6;end=tip+[math.cos(angle)*.03,math.sin(angle)*.03,-.015];line(tip,end,.002);line(end,end+[0,0,-.010],.005,head)
 elif arch=='convallaria':
  for i in range(int(rng.integers(2,5))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.2,.8);line(root,base,.002)
   for sign in (-1,1):leaf(base,d*sign*.4+[0,0,1.],h*rng.uniform(.8,1.),h*.10)
   tip=base+d*h*.35+[0,0,h*.78];ps=curve(base,tip,h*.13,.002)
   for j in range(3,9):
    at=ps[j];end=at+d*.015+[0,0,-.010];line(at,end,.002);line(end,end+[0,0,-.006],.004,head)
 elif arch=='oxalis':
  for i in range(int(rng.integers(5,9))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.25,1.);line(root,base,.002)
   tip=base+[0,0,h*rng.uniform(.5,.8)];line(base,tip,.002)
   for k in range(3):
    angle=theta+k*math.tau/3
    for offset in (-.30,.30):leaf(tip,[math.cos(angle+offset),math.sin(angle+offset),0],h*.25,h*.08)
   if i%3==0:
    flower=base+d*h*.10+[0,0,h*rng.uniform(.85,1.)];line(base,flower,.002)
    for k in range(5):
     angle=k*math.tau/5;line(flower,flower+[math.cos(angle)*.008,math.sin(angle)*.008,.003],.003,head)
 elif arch=='anemone':
  for i in range(int(rng.integers(3,6))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.25,.9);line(root,base,.003)
   tip=base+d*h*.12+[0,0,h*rng.uniform(.8,1.)];line(base,tip,.004);whorl=base+(tip-base)*.6
   for k in range(3):
    angle=theta+k*math.tau/3;outward=np.array([math.cos(angle),math.sin(angle),.1]);petiole=whorl+outward*h*.12;line(whorl,petiole,.003)
    for offset in (-.55,0,.55):leaf(petiole,[math.cos(angle+offset),math.sin(angle+offset),.1],h*.28,h*.04)
   for k in range(6):
    angle=k*math.tau/6+theta;radius=max(.015,h*.105);line(tip,tip+[math.cos(angle)*radius,math.sin(angle)*radius,.003],.005,head)
 elif arch=='wildgarlic':
  for i in range(int(rng.integers(3,6))):
   theta=i*2.399;d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*rng.uniform(.3,.8);line(root,base,.003)
   for sign in (-1,1):leaf(base,d*sign+[0,0,.95],h*.65,h*.065)
   tip=base+d*h*.07+[0,0,h*rng.uniform(.8,1.)];line(base,tip,.004)
   for k in range(7):
    angle=k*math.tau/7+theta;radius=min(.065,h*.20)*rng.uniform(.65,1.);end=tip+[math.cos(angle)*radius,math.sin(angle)*radius,radius*rng.uniform(.15,.65)]
    line(tip,end,.003);line(end,end+[0,0,.008],.006,head)
 elif arch=='trillium':
  # One unbranched stem, one whorl of three bracts, and three white petals.
  theta=rng.uniform(0,math.tau);d=np.array([math.cos(theta),math.sin(theta),0]);tip=root+d*h*.10+[0,0,h];line(root,tip,.005)
  whorl=root+(tip-root)*.68
  for k in range(3):
   angle=theta+k*math.tau/3;direction=np.array([math.cos(angle),math.sin(angle),.12]);leaf(whorl,direction,h*.40,h*.15)
   petal_angle=angle+.35;radius=min(.055,h*.16);end=tip+[math.cos(petal_angle)*radius,math.sin(petal_angle)*radius,radius*.22]
   line(tip,end,radius*.29,head)
 else:
  count=1 if arch=='trillium' else int(rng.integers(3,7))
  for i in range(count):
   theta=i*2.399+rng.uniform(0,math.tau);d=np.array([math.cos(theta),math.sin(theta),0]);base=root+d*spread*.25;line(root,base)
   tip=base+d*spread*.25+[0,0,h*rng.uniform(.75,1)]
   if arch in ('bells','panicleherb'):tip+=d*spread*.3+[0,0,-h*.12]
   ps=curve(base,tip,h*.12 if arch in ('bells','panicleherb') else 0)
   if arch=='panicleherb':
    for j in range(2,8):leaf(ps[j],d*(1 if j%2 else -1)+[0,0,.4],h*.24,h*.06)
   elif arch in ('opposite','spike','compoundherb'):
    for j in (2,4,6):
     for sign in (-1,1):leaf(ps[j],d*sign+[0,0,.35],h*.28,h*.07)
   elif arch in ('palmate','trifoliate','trillium'):
    at=ps[4];num=3 if arch in ('trifoliate','trillium') else 5
    for k in range(num):
     angle=k*math.tau/num+theta;leaf(at,[math.cos(angle),math.sin(angle),.18],h*.38,h*.12)
   else:
    for k in range(5):
     angle=k*2.4;leaf(base,[math.cos(angle),math.sin(angle),.55],h*.55,h*(.12 if arch in ('rosette','arrow') else .055))
   if r['profile'] in SPRING_FLOWERS:
    if arch=='bells':
     for j in (5,6,7,8):
      at=ps[j];end=at+d*h*.10+[0,0,-h*.04];line(at,end)
      # Pendulous tubular flowers sit on the outside of the bent raceme.
      # At 25 mm a true 15-20 mm corolla occupies approximately one cell.
      length=min(.025,h*.09);line(end,end+[0,0,-length],.008,head)
    elif arch=='panicleherb':
     top=tip+[0,0,h*.16];line(tip,top)
     for j in range(1,5):
      center=tip+(top-tip)*j/5
      for k in range(3):
       angle=k*math.tau/3+j;end=center+[math.cos(angle)*h*.09*(1-j/6),math.sin(angle)*h*.09*(1-j/6),h*.025]
       line(center,end);line(end,end+[0,0,.01],.009,head)
    elif arch=='umbel':
     for k in range(6):
      angle=k*math.tau/6;end=tip+[math.cos(angle)*h*.15,math.sin(angle)*h*.15,h*.04];line(tip,end);bloom(end,.6)
    else:bloom(tip)
 points=np.array([x for a,b,w,m in segments for x in (a,b)]);pad=max(w for a,b,w,m in segments)+pitch*2
 lo=np.floor((points.min(0)-pad)/pitch).astype(int);lo[2]=0;hi=np.ceil((points.max(0)+pad)/pitch).astype(int)
 if np.prod(hi-lo+1)>120_000_000:raise ValueError('Understory grid exceeds 120 MB limit')
 g=VoxelGrid(tuple(hi-lo+1),lo,pitch)
 for a,b,w,m in sorted(segments,key=lambda segment: segment[3]==wood):g.capsule(a/pitch-lo,b/pitch-lo,w/pitch,w/pitch,m)
 g=g.crop();occ=g.data>0;labels,count=ndimage.label(occ,structure=np.ones((3,3,3)));sizes=np.bincount(labels.ravel());sizes[0]=0
 detached=int(occ.sum()-sizes.max());dims=np.array(g.shape)*pitch
 woodmask=g.data==wood;wood_connected=g.component_fraction(woodmask,connectivity=1) if woodmask.any() else 1.
 wood_detached=int(round(int(woodmask.sum())*(1-wood_connected))) if woody else 0
 stats=dict(seed=seed,spec_hash=spec_hash(spec),kind=spec['kind'],generator=VERSION,profile=r['profile'],architecture=arch,size_class=size,growth_form=form,voxel_cm=pitch*100,height_m=round(float(dims[2]),3),length_m=round(float(dims[0]),3),footprint_m=dims[:2].tolist(),extent_vox=list(g.shape),voxels=g.count(),by_material=g.histogram(),wood_detached=wood_detached,wood_connected=wood_connected,detached=detached,attached_frac=float(sizes.max()/max(1,occ.sum())),ground_contact=int(occ[:,:,0].sum()),max_order=3 if woody else 0,clumps=leafcount,nodes=len(segments)+1,segments=len(segments),grid_mb=round(g.data.nbytes/1e6,2),ms_total=round((time.perf_counter()-start)*1000,1),season='spring')
 return Asset(g,None,spec,seed,dict(spec,height_m=h,plant_recipe=dict(r,size=size,form=form)),stats)

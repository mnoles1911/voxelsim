"""Orthographic RGB views shared by the reference-mesh creature tools."""
import numpy as np
from PIL import Image


def ortho(rgb,occ,axis,reverse=False,size=700):
    colors=np.moveaxis(rgb,axis,0); solid=np.moveaxis(occ,axis,0)
    if reverse:colors=colors[::-1];solid=solid[::-1]
    idx=solid.argmax(0);u,v=np.indices(idx.shape)
    pixels=colors[idx,u,v].copy();mask=solid.any(0)
    pixels[~mask]=[223,226,229]
    if axis!=2:pixels=np.flip(pixels.transpose(1,0,2),axis=0)
    else:pixels=pixels.transpose(1,0,2)
    if reverse and axis!=2:pixels=pixels[:,::-1]
    img=Image.fromarray(pixels)
    img.thumbnail((size,size),Image.Resampling.NEAREST) if max(img.size)>size else None
    scale=max(1,size//max(img.size));img=img.resize((img.width*scale,img.height*scale),Image.Resampling.NEAREST)
    canvas=Image.new('RGB',(size,size),(223,226,229));canvas.paste(img,((size-img.width)//2,(size-img.height)//2))
    return canvas

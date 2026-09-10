#pragma once
#include "voxelcore/assetbank.h"

namespace vxc {
// Parsed host input. Authored seed numbers are deliberately absent: filenames
// and source identities bind to the accepted bank, which supplies runtime slots.
struct EcoNamedVariant {
    EcoVariant placement;
    std::string filename;
    std::string sourceIdentity;
};
struct EcoNamedSpecies {
    std::string name;
    AssetKind kind=AssetKind::kTree;
    EcoSpeciesProfile placement;
    std::vector<EcoNamedVariant> variants;
};
// VerifyIdentity must consult the exact accepted-byte observer attached to this
// bank library, not reread a file (which may have changed after grid parsing).
// The result is all-or-nothing and is installed before any worldgen worker runs.
template<class VerifyIdentity>
bool ecoBindPublished(const EcoConfig& fields,uint16_t biomeMask,
    const std::vector<EcoNamedSpecies>& named,const AssetManifest& manifest,
    const AssetBankLibrary& banks,VerifyIdentity&& verifyIdentity,
    EcoPlacementConfig& output,std::string& error){
    output={};error.clear();
    auto fail=[&](const std::string& why){error=why;return false;};
    if(!ecoConfigValid(fields)||named.empty()||named.size()>4096)
        return fail("invalid ecological field/species configuration");
    EcoPlacementConfig built;built.fields=fields;built.biomeMask=biomeMask;
    for(const auto& source:named){
        size_t bankId=0;
        while(bankId<manifest.species().size()&&manifest.species()[bankId].name!=source.name)++bankId;
        if(bankId>=manifest.species().size()||bankId>UINT16_MAX)
            return fail("species absent from bank manifest: "+source.name);
        if(manifest.species()[bankId].kind!=source.kind||
           (source.kind==AssetKind::kTree)!=source.placement.tree)
            return fail("ecological kind disagrees with bank manifest: "+source.name);
        EcoSpeciesProfile profile=source.placement;
        profile.bankId=uint16_t(bankId);profile.variants.clear();
        if(source.variants.empty()||source.variants.size()>65536)
            return fail("invalid variant count: "+source.name);
        for(const auto& entry:source.variants){
            uint16_t slot=0;const AssetGrid* grid=nullptr;
            if(!banks.findSeedByFilename(profile.bankId,entry.filename,slot,grid)||!grid)
                return fail("named variant not accepted by bank: "+entry.filename);
            if(entry.sourceIdentity.empty()||!verifyIdentity(*grid,entry.sourceIdentity))
                return fail("accepted source identity not published: "+entry.filename);
            auto variant=entry.placement;
            variant.seedIndex=slot;variant.published=true;
            // Declared dimensions must match the actual grid. The host may
            // author crown influence smaller than the full bounding box.
            const auto pitch=grid->voxelSizeMm();
            if((pitch!=25&&pitch!=50&&pitch!=100)||
               int64_t(grid->sizeZ())*pitch!=variant.heightMm)
                return fail("variant height/pitch disagrees with accepted grid: "+entry.filename);
            profile.variants.push_back(variant);
        }
        built.species.push_back(std::move(profile));
    }
    if(!built.valid())return fail("invalid or duplicate ecological species/variant traits");
    output=std::move(built);return true;
}
} // namespace vxc

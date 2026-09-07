#include "voxelcore/assetslopefit.h"
#include "vxctest.h"
#include <limits>
using namespace vxc;

VXC_TEST(slope_fit_flat_and_negative_world_heights_snap_down) {
    AssetSupportBin b{0, -1050, -1050};
    const auto fit = fitAssetSupports(&b, 1, -1050, 100, 100, 100);
    CHECK(fit.accepted); CHECK_EQ(fit.originZMm, -1100);
    CHECK_EQ(fit.sinkMm, 50);
}
VXC_TEST(slope_fit_downhill_support_is_seated_without_tilting) {
    AssetSupportBin b[]={{0, -300, -100}, {50, 100, 300}};
    const auto fit=fitAssetSupports(b,2,0,100,500,700);
    CHECK(fit.accepted); CHECK_EQ(fit.originZMm,-300);
    for(const auto& s:b) CHECK(fit.originZMm+s.bottomMm<=s.groundLowMm);
}
VXC_TEST(slope_fit_refuses_cliff_even_when_sink_budget_allows_it) {
    AssetSupportBin b[]={{0,-300,-300},{0,1600,1600}};
    CHECK(!fitAssetSupports(b,2,0,100,500,800).accepted);
}
VXC_TEST(slope_fit_refuses_excessive_sink_and_does_not_accumulate_offsets) {
    AssetSupportBin b{0,-900,-900};
    CHECK(!fitAssetSupports(&b,1,0,100,800,1000).accepted);
    const auto first=fitAssetSupports(&b,1,-900,100,800,1000);
    const auto again=fitAssetSupports(&b,1,-900,100,800,1000);
    CHECK(first.accepted); CHECK_EQ(first.originZMm,again.originZMm);
}
VXC_TEST(slope_fit_invalid_support_fails_closed) {
    AssetSupportBin b{0,0,0};
    CHECK(!fitAssetSupports(nullptr,0,0,100,100,100).accepted);
    CHECK(!fitAssetSupports(&b,1,0,0,100,100).accepted);
    b.groundLowMm=std::numeric_limits<double>::quiet_NaN();
    CHECK(!fitAssetSupports(&b,1,0,100,100,100).accepted);
}
VXC_TEST(slope_fit_dense_planar_footprint_envelopes_bound_support) {
    // Different slope headings and uneven root heights: the entire planar bin
    // is bounded by its four corners, not merely a centre-point contact.
    for(int heading=0;heading<32;++heading){
        const double a=std::cos(heading*.2)*.2,b=std::sin(heading*.2)*.2;
        AssetSupportBin bins[16];
        for(int x=0;x<4;++x)for(int y=0;y<4;++y){
            auto& s=bins[x*4+y];s.bottomMm=((x+y)%3)*25.;
            s.groundLowMm=1e9;s.groundHighMm=-1e9;
            for(int dx=0;dx<2;++dx)for(int dy=0;dy<2;++dy){
                const double z=a*((x+dx)*250.-500)+b*((y+dy)*250.-500);
                s.groundLowMm=std::min(s.groundLowMm,z);
                s.groundHighMm=std::max(s.groundHighMm,z);
            }
        }
        const auto fit=fitAssetSupports(bins,16,0,100,500,800);
        CHECK(fit.accepted);
        for(const auto& s:bins)CHECK(fit.originZMm+s.bottomMm<=s.groundLowMm);
    }
}

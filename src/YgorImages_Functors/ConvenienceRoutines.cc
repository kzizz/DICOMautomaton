//ConvenienceRoutines.cc.

#include <exception>
#include <list>
#include <map>
#include <string>
#include <utility>

#include "ConvenienceRoutines.h"
#include "YgorImages.h" 
#include "YgorStats.h"       //Needed for Stats:: namespace.
#include "YgorString.h"


void UpdateImageDescription(
        std::reference_wrapper<planar_image<float,double>> img_refw,
        const std::string &Description){
    img_refw.get().metadata["Description"] = Description;
    return;
}

void UpdateImageDescription(
        planar_image_collection<float,double>::images_list_it_t img_it,
        const std::string &Description){
    return UpdateImageDescription( std::ref(*img_it), Description );
}


void UpdateImageWindowCentreWidth(std::reference_wrapper<planar_image<float,double>> img_refw,
                                  const Stats::Running_MinMax<float> &RMM){
    try{
        const auto Min = RMM.Current_Min();
        const auto Max = RMM.Current_Max();
        const auto Centre =  Min * static_cast<float>(0.5)
                           + Max * static_cast<float>(0.5);
        const auto Width = Max - Min; // Full width.

        img_refw.get().metadata["WindowValidFor"] = img_refw.get().metadata["Description"];
        img_refw.get().metadata["WindowCenter"]   = Xtostring(Centre);
        img_refw.get().metadata["WindowWidth"]    = Xtostring(Width);
 
        img_refw.get().metadata["PixelMinMaxValidFor"] = img_refw.get().metadata["Description"];
        img_refw.get().metadata["PixelMin"]            = Xtostring(Min);
        img_refw.get().metadata["PixelMax"]            = Xtostring(Max);

    }catch(const std::exception &){
        img_refw.get().metadata.erase("WindowValidFor");
        img_refw.get().metadata.erase("PixelMinMaxValidFor");
    }
    return;
}

void UpdateImageWindowCentreWidth(planar_image_collection<float,double>::images_list_it_t img_it,
                                  const Stats::Running_MinMax<float> &RMM){
    return UpdateImageWindowCentreWidth( std::ref(*img_it), RMM );
}

void UpdateImageWindowCentreWidth(std::reference_wrapper<planar_image<float,double>> img_refw){
    Stats::Running_MinMax<float> RMM;
    const auto MM = img_refw.get().minmax();
    RMM.Digest(MM.first);
    RMM.Digest(MM.second);

    UpdateImageWindowCentreWidth(img_refw, RMM);
    return;
}

void UpdateImageWindowCentreWidth(planar_image_collection<float,double>::images_list_it_t img_it){
    return UpdateImageWindowCentreWidth( std::ref(*img_it) );
}

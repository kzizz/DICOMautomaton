//DrawGeometry.cc - A part of DICOMautomaton 2019. Written by hal clark.

#include <asio.hpp>
#include <algorithm>
#include <experimental/optional>
#include <fstream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set> 
#include <stdexcept>
#include <string>    
#include <utility>            //Needed for std::pair.
#include <vector>

#include "Explicator.h"       //Needed for Explicator class.
#include "YgorImages.h"
#include "YgorMath.h"         //Needed for vec3 class.
#include "YgorMisc.h"         //Needed for FUNCINFO, FUNCWARN, FUNCERR macros.
#include "YgorStats.h"        //Needed for Stats:: namespace.
#include "YgorString.h"       //Needed for GetFirstRegex(...)

#include "../Structs.h"
#include "../Regex_Selectors.h"
#include "../Thread_Pool.h"
#include "../YgorImages_Functors/ConvenienceRoutines.h"

#include "../Surface_Meshes.h"

#include "DrawGeometry.h"



OperationDoc OpArgDocDrawGeometry(void){
    OperationDoc out;
    out.name = "DrawGeometry";

    out.desc = 
        "This operation draws shapes and patterns on images."
        " Drawing is confined to one or more ROIs.";
        
    
    out.args.emplace_back();
    out.args.back() = IAWhitelistOpArgDoc();
    out.args.back().name = "ImageSelection";
    out.args.back().default_val = "last";

    out.args.emplace_back();
    out.args.back().name = "VoxelValue";
    out.args.back().desc = "The value to give voxels which are coincident with a point from the point cloud.";
    out.args.back().default_val = "1.0";
    out.args.back().expected = true;
    out.args.back().examples = { "-1.0", "0.0", "1.23", "nan", "inf" };

    out.args.emplace_back();
    out.args.back().name = "Overwrite";
    out.args.back().desc = "Whether to overwrite voxels interior or exterior to the specified ROI(s).";
    out.args.back().default_val = "interior";
    out.args.back().expected = true;
    out.args.back().examples = { "interior", "exterior" };

    out.args.emplace_back();
    out.args.back().name = "Channel";
    out.args.back().desc = "The image channel to use. Zero-based.";
    out.args.back().default_val = "0";
    out.args.back().expected = true;
    out.args.back().examples = { "0", "1", "2" };

    out.args.emplace_back();
    out.args.back().name = "NormalizedROILabelRegex";
    out.args.back().desc = "A regex matching ROI labels/names to consider. The default will match"
                           " all available ROIs. Be aware that input spaces are trimmed to a single space."
                           " If your ROI name has more than two sequential spaces, use regex to avoid them."
                           " All ROIs have to match the single regex, so use the 'or' token if needed."
                           " Regex is case insensitive and uses extended POSIX syntax.";
    out.args.back().default_val = ".*";
    out.args.back().expected = true;
    out.args.back().examples = { ".*", ".*Body.*", "Body", "Gross_Liver",
                            R"***(.*Left.*Parotid.*|.*Right.*Parotid.*|.*Eye.*)***",
                            R"***(Left Parotid|Right Parotid)***" };

    out.args.emplace_back();
    out.args.back().name = "ROILabelRegex";
    out.args.back().desc = "A regex matching ROI labels/names to consider. The default will match"
                           " all available ROIs. Be aware that input spaces are trimmed to a single space."
                           " If your ROI name has more than two sequential spaces, use regex to avoid them."
                           " All ROIs have to match the single regex, so use the 'or' token if needed."
                           " Regex is case insensitive and uses extended POSIX syntax.";
    out.args.back().default_val = ".*";
    out.args.back().expected = true;
    out.args.back().examples = { ".*", ".*body.*", "body", "Gross_Liver",
                                 R"***(.*left.*parotid.*|.*right.*parotid.*|.*eyes.*)***",
                                 R"***(left_parotid|right_parotid)***" };

    out.args.emplace_back();
    out.args.back().name = "ContourOverlap";
    out.args.back().desc = "Controls overlapping contours are treated."
                           " The default 'ignore' treats overlapping contours as a single contour, regardless of"
                           " contour orientation. The option 'honour_opposite_orientations' makes overlapping contours"
                           " with opposite orientation cancel. Otherwise, orientation is ignored. The latter is useful"
                           " for Boolean structures where contour orientation is significant for interior contours (holes)."
                           " The option 'overlapping_contours_cancel' ignores orientation and cancels all contour overlap.";
    out.args.back().default_val = "ignore";
    out.args.back().expected = true;
    out.args.back().examples = { "ignore", "honour_opposite_orientations", 
                            "overlapping_contours_cancel", "honour_opps", "overlap_cancel" }; 

    out.args.emplace_back();
    out.args.back().name = "Inclusivity";
    out.args.back().desc = "Controls how voxels are deemed to be 'within' the interior of the selected ROI(s)."
                           " The default 'center' considers only the central-most point of each voxel."
                           " There are two corner options that correspond to a 2D projection of the voxel onto the image plane."
                           " The first, 'planar_corner_inclusive', considers a voxel interior if ANY corner is interior."
                           " The second, 'planar_corner_exclusive', considers a voxel interior if ALL (four) corners are interior.";
    out.args.back().default_val = "center";
    out.args.back().expected = true;
    out.args.back().examples = { "center", "centre", 
                                 "planar_corner_inclusive", "planar_inc",
                                 "planar_corner_exclusive", "planar_exc" };

    return out;
}



Drover DrawGeometry(Drover DICOM_data, OperationArgPkg OptArgs, std::map<std::string,std::string> /*InvocationMetadata*/, std::string FilenameLex){

    Explicator X(FilenameLex);

    //---------------------------------------------- User Parameters --------------------------------------------------
    const auto ImageSelectionStr = OptArgs.getValueStr("ImageSelection").value();

    const auto VoxelValue = std::stod( OptArgs.getValueStr("VoxelValue").value() );
    const auto Channel = std::stol( OptArgs.getValueStr("Channel").value() );
    const auto OverwriteStr = OptArgs.getValueStr("Overwrite").value();

    const auto InclusivityStr = OptArgs.getValueStr("Inclusivity").value();
    const auto ContourOverlapStr = OptArgs.getValueStr("ContourOverlap").value();

    const auto NormalizedROILabelRegex = OptArgs.getValueStr("NormalizedROILabelRegex").value();
    const auto ROILabelRegex = OptArgs.getValueStr("ROILabelRegex").value();

    //-----------------------------------------------------------------------------------------------------------------

    const auto regex_centre = Compile_Regex("^cent.*");
    const auto regex_pci = Compile_Regex("^planar_?c?o?r?n?e?r?s?_?inc?l?u?s?i?v?e?$");
    const auto regex_pce = Compile_Regex("^planar_?c?o?r?n?e?r?s?_?exc?l?u?s?i?v?e?$");

    const auto regex_interior = Compile_Regex("^int?e?r?i?o?r?$");
    const auto regex_exterior = Compile_Regex("^ext?e?r?i?o?r?$");

    const auto regex_ignore = Compile_Regex("^ig?n?o?r?e?$");
    const auto regex_honopps = Compile_Regex("^ho?n?o?u?r?_?o?p?p?o?s?i?t?e?_?o?r?i?e?n?t?a?t?i?o?n?s?$");
    const auto regex_cancel = Compile_Regex("^ov?e?r?l?a?p?p?i?n?g?_?c?o?n?t?o?u?r?s?_?c?a?n?c?e?l?s?$");

    const auto ShouldOverwriteExterior = std::regex_match(OverwriteStr, regex_exterior);
    const auto ShouldOverwriteInterior = std::regex_match(OverwriteStr, regex_interior);
    if(!ShouldOverwriteExterior && !ShouldOverwriteInterior){
        throw std::invalid_argument("Nothing will be overwritten. Refusing to continue.");
    }

    std::function<void(long int, long int, long int, std::reference_wrapper<planar_image<float,double>>, float &)> f_noop;

    //-----------------------------------------------------------------------------------------------------------------


    // Gather contours.
    auto cc_all = All_CCs( DICOM_data );
    auto cc_ROIs = Whitelist( cc_all, { { "ROIName", ROILabelRegex },
                                        { "NormalizedROIName", NormalizedROILabelRegex } } );
    if(cc_ROIs.empty()){
        throw std::invalid_argument("No contours selected. Cannot continue.");
    }


    auto IAs_all = All_IAs( DICOM_data );
    auto IAs = Whitelist( IAs_all, ImageSelectionStr );
    for(auto & iap_it : IAs){
        if((*iap_it)->imagecoll.images.empty()) continue;

        // Used to determine image characteristics.
        std::reference_wrapper<planar_image<float,double>> img_refw = std::ref(*(std::begin((*iap_it)->imagecoll.images)));

        ////////////////////////////////////////////////////////////
        // Grid pattern.
        std::vector<line<double>> grid_lines;
        const double grid_sep = 15.0;
        const double grid_rad = 1.5;
        {

            const auto img_origin = img_refw.get().anchor + img_refw.get().offset;

            const auto img_unit_x = img_refw.get().row_unit;
            const auto img_unit_y = img_refw.get().col_unit;
            const auto img_unit_z = img_unit_x.Cross( img_unit_y );
            const auto img_centre = (*iap_it)->imagecoll.center();

            //const auto img_span_x = img_unit_x.Dot( img_centre - img_origin ) * 2.0;
            //const auto img_span_y = img_unit_y.Dot( img_centre - img_origin ) * 2.0;
            //const auto img_span_z = img_unit_z.Dot( img_centre - img_origin ) * 2.0;

            //const auto grid_origin = img_centre; // + (img_centre - img_origin).unit() * grid_sep * 0.25;
            const auto grid_origin = img_centre; // Note: changing this will require changing N_lines below!

            auto unit_x = img_unit_x.rotate_around_y(M_PI*0.05).rotate_around_z( M_PI*0.03);
            auto unit_y = img_unit_y.rotate_around_z(M_PI*0.15).rotate_around_x( M_PI*0.05);
            auto unit_z = img_unit_z.rotate_around_x(M_PI*0.25).rotate_around_y(-M_PI*0.07);
            if(!unit_x.GramSchmidt_orthogonalize(unit_y, unit_z)){
                throw std::invalid_argument("Cannot orthogonalize grid unit vectors. Cannot continue.");
            }

            // Ensure the image will be tiled with grid lines by ensuring the maximum spatial extent will be covered no
            // matter how the grid is oriented.
            const auto img_halfspan = (img_centre - img_origin).length();
            const long int N_lines = static_cast<long int>(std::ceil(img_halfspan / grid_sep));

            // Create the grid lines.
            for(long int y = -N_lines; y <= N_lines; ++y){
                for(long int z = -N_lines; z <= N_lines; ++z){
                    const auto A = grid_origin + (unit_y * grid_sep * (y * 1.0))
                                               + (unit_z * grid_sep * (z * 1.0));
                    const auto B = A + unit_x;
                    grid_lines.emplace_back(A, B);
                }
            }
            for(long int x = -N_lines; x <= N_lines; ++x){
                for(long int y = -N_lines; y <= N_lines; ++y){
                    const auto A = grid_origin + (unit_x * grid_sep * (x * 1.0))
                                               + (unit_y * grid_sep * (y * 1.0));
                    const auto B = A + unit_z;
                    grid_lines.emplace_back(A, B);
                }
            }
            for(long int x = -N_lines; x <= N_lines; ++x){
                for(long int z = -N_lines; z <= N_lines; ++z){
                    const auto A = grid_origin + (unit_x * grid_sep * (x * 1.0))
                                               + (unit_z * grid_sep * (z * 1.0));
                    const auto B = A + unit_y;
                    grid_lines.emplace_back(A, B);
                }
            }
        }
        ////////////////////////////////////////////////////////////

        PartitionedImageVoxelVisitorMutatorUserData ud;

        ud.mutation_opts.editstyle = Mutate_Voxels_Opts::EditStyle::InPlace;
        ud.mutation_opts.aggregate = Mutate_Voxels_Opts::Aggregate::First;
        ud.mutation_opts.adjacency = Mutate_Voxels_Opts::Adjacency::SingleVoxel;
        ud.mutation_opts.maskmod   = Mutate_Voxels_Opts::MaskMod::Noop;
        ud.description = "Drawn geometry";

        if(false){
        }else if( std::regex_match(ContourOverlapStr, regex_ignore) ){
            ud.mutation_opts.contouroverlap = Mutate_Voxels_Opts::ContourOverlap::Ignore;
        }else if( std::regex_match(ContourOverlapStr, regex_honopps) ){
            ud.mutation_opts.contouroverlap = Mutate_Voxels_Opts::ContourOverlap::HonourOppositeOrientations;
        }else if( std::regex_match(ContourOverlapStr, regex_cancel) ){
            ud.mutation_opts.contouroverlap = Mutate_Voxels_Opts::ContourOverlap::ImplicitOrientations;
        }else{
            throw std::invalid_argument("ContourOverlap argument '"_s + ContourOverlapStr + "' is not valid");
        }
        if(false){
        }else if( std::regex_match(InclusivityStr, regex_centre) ){
            ud.mutation_opts.inclusivity = Mutate_Voxels_Opts::Inclusivity::Centre;
        }else if( std::regex_match(InclusivityStr, regex_pci) ){
            ud.mutation_opts.inclusivity = Mutate_Voxels_Opts::Inclusivity::Inclusive;
        }else if( std::regex_match(InclusivityStr, regex_pce) ){
            ud.mutation_opts.inclusivity = Mutate_Voxels_Opts::Inclusivity::Exclusive;
        }else{
            throw std::invalid_argument("Inclusivity argument '"_s + InclusivityStr + "' is not valid");
        }

        // Create a functor for the specific geometry / shape.
        std::function<void(long int, long int, long int, std::reference_wrapper<planar_image<float,double>>, float &)> f_overwrite;


        // Grid pattern.
        if(true){
            f_overwrite = [&]( long int row,
                               long int col,
                               long int chan,
                               std::reference_wrapper<planar_image<float,double>> img_refw,
                               float &voxel_val ) -> void {
                if( (Channel < 0) || (Channel == chan) ){
                    const auto pos = img_refw.get().position(row,col);

                    for(const auto &l : grid_lines){
                        const auto dist = l.Distance_To_Point(pos);
                        if(dist < grid_rad){
                            voxel_val = VoxelValue;
                            return;
                        }
                    }
                    voxel_val = 0.0f;
                    return;
                }
            };
        }

        // Enable the functor as necessary.
        ud.f_bounded = f_noop;
        ud.f_unbounded = f_noop;
        ud.f_visitor = f_noop;
        if(ShouldOverwriteInterior) ud.f_bounded = f_overwrite;
        if(ShouldOverwriteExterior) ud.f_unbounded = f_overwrite;

        if(!(*iap_it)->imagecoll.Process_Images_Parallel( GroupIndividualImages,
                                                          PartitionedImageVoxelVisitorMutator,
                                                          {}, cc_ROIs, &ud )){
            throw std::runtime_error("Unable to draw geometry within the specified ROI(s).");
        }
    }

    return DICOM_data;
}


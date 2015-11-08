
#include <list>
#include <functional>
#include <limits>
#include <map>
#include <cmath>
#include <experimental/any>

#include <pqxx/pqxx>         //PostgreSQL C++ interface.
#include <jansson.h>         //For JSON handling.

#include <nlopt.h>

#include "YgorMisc.h"
#include "YgorMath.h"
#include "YgorImages.h"
#include "YgorStats.h"       //Needed for Stats:: namespace.
#include "YgorFilesDirs.h"   //Needed for Does_File_Exist_And_Can_Be_Read(...), etc..
#include "YgorAlgorithms.h"  //Needed for For_Each_In_Parallel<..>(...)
#include "YgorString.h"      //Needed for GetFirstRegex(...)
#include "YgorPlot.h"

#include "Liver_Pharmacokinetic_Model.h"

#include "../Compute/Per_ROI_Time_Courses.h"
//struct ComputePerROITimeCoursesUserData;


//This is the function we will minimize: the sum of the squared residuals between the ROI measured
// concentrations and the total concentration predicted by the fitted model.
samples_1D<double> *theAIF;    //Global, but only so we can pass a c-style function to nlopt.
samples_1D<double> *theVIF;    //Global, but only so we can pass a c-style function to nlopt.
samples_1D<double> *theROI;    //Global, but only so we can pass a c-style function to nlopt.
double func_to_min(unsigned, const double *params, double *grad, void *){
    if(grad != nullptr) FUNCERR("NLOpt asking for a gradient. We don't have it at the moment...");
    const double k1A  = params[0];
    const double tauA = params[1];
    const double k2V  = params[2];
    const double tauV = params[3];
    const double k2   = params[4];

    double sqDist = 0.0;
    for(const auto &P : theROI->samples){
        const double t = P[0];
        const double Fexpdata = P[2];

        //-------------------------------------------------------------------------------------------------------------------------
        //First, the arterial contribution. This involves an integral over the AIF.
        // Compute: \int_{tau=0}^{tau=t} k1A * AIF(tau - tauA) * exp((k2)*(tau-t)) dtau 
        //          = k1A \int_{tau=-tauA}^{tau=(t-tauA)} AIF(tau) * exp((k2)*(tau-(t-tauA))) dtau.
        // The integration coordinate is transformed to make it suit the integration-over-kernel-... implementation. 
        //
        double Integratedk1ACA = k1A*theAIF->Integrate_Over_Kernel_exp(-tauA, t-tauA, {k2,0.0}, {-(t-tauA),0.0})[0];
    
        //-------------------------------------------------------------------------------------------------------------------------
        //The venous contribution is identical, but all the fitting parameters are different and AIF -> VIF.
        //
        double Integratedk2VCV = k2V*theVIF->Integrate_Over_Kernel_exp(-tauV, t-tauV, {k2,0.0}, {-(t-tauV),0.0})[0];
        const double Ffitfunc = Integratedk1ACA + Integratedk2VCV;

        //Standard L2-norm.
        sqDist += std::pow(Fexpdata - Ffitfunc, 2.0);
    }
    return sqDist;
}
 



bool LiverPharmacoModel(planar_image_collection<float,double>::images_list_it_t first_img_it,
                        std::list<planar_image_collection<float,double>::images_list_it_t> selected_img_its,
                        std::list<std::reference_wrapper<contour_collection<double>>> ccsl,
                        std::experimental::any user_data ){


    //This takes aggregate time courses for (1) hepatic portal vein and (2) ascending aorta and attempts to 
    // fit a pharmacokinetic model to each voxel within the provided gross liver ROI. This entails fitting
    // a convolution model to the data, and a general optimization procedure is used.
    //
    // The input images must be grouped in the same way that the ROI time courses were computed. This will
    // most likely mean grouping spatially-overlapping images that have identical 'image acquisition time' 
    // (or just 'dt' for short) together.

    ComputePerROITimeCoursesUserData *user_data_s;
    try{
        user_data_s = std::experimental::any_cast<ComputePerROITimeCoursesUserData *>(user_data);
    }catch(const std::exception &e){
        FUNCWARN("Unable to cast user_data to appropriate format. Cannot continue with computation");
        return false;
    }

    if( (user_data_s->time_courses.count("Abdominal_Aorta") == 0 )
    ||  (user_data_s->time_courses.count("Hepatic_Portal_Vein") == 0 ) ){
        throw std::invalid_argument("Both arterial and venous input time courses are needed."
                                    "(Are they named differently to the hard-coded names?)");
    }

    //Get convenient handles for the arterial and venous input time courses.
    //
    // NOTE: "Because the contrast agent does not enter the RBCs, the time series Caorta(t) and Cportal(t)
    //       were divided by one minus the hematocrit." (From Van Beers et al. 2000.)
    auto Carterial = user_data_s->time_courses["Abdominal_Aorta"].Multiply_With(1.0/(1.0 - 0.42));
    auto Cvenous   = user_data_s->time_courses["Hepatic_Portal_Vein"].Multiply_With(1.0/(1.0 - 0.42));

    theAIF = &Carterial;
    theVIF = &Cvenous;

    //Trim all but the liver contour. 
    //
    //   TODO: hoist out of this function, and provide a convenience
    //         function called something like: Prune_Contours_Other_Than(cc_all, "Liver_Rough");
    //         You could do regex or whatever is needed.

    ccsl.remove_if([](std::reference_wrapper<contour_collection<double>> cc) -> bool {
                       const auto ROINameOpt = cc.get().contours.front().GetMetadataValueAs<std::string>("ROIName");
                       const auto ROIName = ROINameOpt.value();
                       return (ROIName != "Suspected_Liver_Rough");
                   });


    //This routine performs a number of calculations. It is experimental and excerpts you plan to rely on should be
    // made into their own analysis functors.
    const bool InhibitSort = true; //Disable continuous sorting (defer to single sort later) to speed up data ingress.


    //Figure out if there are any contours for which are within the spatial extent of the image. 
    // There are many ways to do this! Since we are merely highlighting the contours, we scan 
    // all specified collections and treat them homogeneously.
    //
    // NOTE: We only bother to grab individual contours here. You could alter this if you wanted 
    //       each contour_collection's contours to have an identifying colour.
    //if(ccsl.empty()){
    if(ccsl.size() != 1){
        FUNCWARN("Missing needed contour information. Cannot continue with computation");
        return false;
    }

/*
    //typedef std::list<contour_of_points<double>>::iterator contour_iter;
    //std::list<contour_iter> rois;
    decltype(ccsl) rois;
    for(auto &ccs : ccsl){
        for(auto it =  ccs.get().contours.begin(); it != ccs.get().contours.end(); ++it){
            if(it->points.empty()) continue;
            //if(first_img_it->encompasses_contour_of_points(*it)) rois.push_back(it);
            if(first_img_it->encompasses_contour_of_points(*it)) rois.push_back(
        }
    }
*/
    //Make a 'working' image which we can edit. Start by duplicating the first image.
    planar_image<float,double> working;
    working = *first_img_it;

    //Paint all pixels black.
    working.fill_pixels(static_cast<float>(0));

    //Loop over the rois, rows, columns, channels, and finally any selected images (if applicable).
    const auto row_unit   = first_img_it->row_unit;
    const auto col_unit   = first_img_it->col_unit;
    const auto ortho_unit = row_unit.Cross( col_unit ).unit();

    size_t Minimization_Failure_Count = 0;


    //Record the min and max actual pixel values for windowing purposes.
    float curr_min_pixel = std::numeric_limits<float>::max();
    float curr_max_pixel = std::numeric_limits<float>::min();

    //Loop over the ccsl, rois, rows, columns, channels, and finally any selected images (if applicable).
    //for(const auto &roi : rois){
    for(auto &ccs : ccsl){
        for(auto roi_it = ccs.get().contours.begin(); roi_it != ccs.get().contours.end(); ++roi_it){
            if(roi_it->points.empty()) continue;
            //if(first_img_it->encompasses_contour_of_points(*it)) rois.push_back(it);

            //auto roi = *it;
            if(! first_img_it->encompasses_contour_of_points(*roi_it)) continue;

            const auto ROIName =  roi_it->GetMetadataValueAs<std::string>("ROIName");
            if(!ROIName){
                FUNCWARN("Missing necessary tags for reporting analysis results. Cannot continue");
                return false;
            }
            
    /*
            //Try figure out the contour's name.
            const auto StudyInstanceUID = roi_it->GetMetadataValueAs<std::string>("StudyInstanceUID");
            const auto ROIName =  roi_it->GetMetadataValueAs<std::string>("ROIName");
            const auto FrameofReferenceUID = roi_it->GetMetadataValueAs<std::string>("FrameofReferenceUID");
            if(!StudyInstanceUID || !ROIName || !FrameofReferenceUID){
                FUNCWARN("Missing necessary tags for reporting analysis results. Cannot continue");
                return false;
            }
            const analysis_key_t BaseAnalysisKey = { {"StudyInstanceUID", StudyInstanceUID.value()},
                                                     {"ROIName", ROIName.value()},
                                                     {"FrameofReferenceUID", FrameofReferenceUID.value()},
                                                     {"SpatialBoxr", Xtostring(boxr)},
                                                     {"MinimumDatum", Xtostring(min_datum)} };
            //const auto ROIName = ReplaceAllInstances(roi_it->metadata["ROIName"], "[_]", " ");
            //const auto ROIName = roi_it->metadata["ROIName"];
    */
    
    /*
            //Construct a bounding box to reduce computational demand of checking every voxel.
            auto BBox = roi_it->Bounding_Box_Along(row_unit, 1.0);
            auto BBoxBestFitPlane = BBox.Least_Squares_Best_Fit_Plane(vec3<double>(0.0,0.0,1.0));
            auto BBoxProjectedContour = BBox.Project_Onto_Plane_Orthogonally(BBoxBestFitPlane);
            const bool BBoxAlreadyProjected = true;
    */
    
            //Prepare a contour for fast is-point-within-the-polygon checking.
            auto BestFitPlane = roi_it->Least_Squares_Best_Fit_Plane(ortho_unit);
            auto ProjectedContour = roi_it->Project_Onto_Plane_Orthogonally(BestFitPlane);
            const bool AlreadyProjected = true;
    
            for(auto row = 0; row < first_img_it->rows; ++row){
                for(auto col = 0; col < first_img_it->columns; ++col){
                    //Figure out the spatial location of the present voxel.
                    const auto point = first_img_it->position(row,col);
    
    /*
                    //Check if within the bounding box. It will generally be cheaper than the full contour (4 points vs. ? points).
                    auto BBoxProjectedPoint = BBoxBestFitPlane.Project_Onto_Plane_Orthogonally(point);
                    if(!BBoxProjectedContour.Is_Point_In_Polygon_Projected_Orthogonally(BBoxBestFitPlane,
                                                                                        BBoxProjectedPoint,
                                                                                        BBoxAlreadyProjected)) continue;
    */
    
                    //Perform a more detailed check to see if we are in the ROI.
                    auto ProjectedPoint = BestFitPlane.Project_Onto_Plane_Orthogonally(point);
                    if(ProjectedContour.Is_Point_In_Polygon_Projected_Orthogonally(BestFitPlane,
                                                                                   ProjectedPoint,
                                                                                   AlreadyProjected)){
                        for(auto chan = 0; chan < first_img_it->channels; ++chan){
                            //Check if another ROI has already written to this voxel. Bail if so.
                            {
                                const auto curr_val = working.value(row, col, chan);
                                if(curr_val != 0) FUNCERR("There are overlapping ROIs. This code currently cannot handle this. "
                                                          "You will need to run the functor individually on the overlapping ROIs.");
                            }
    
                            //Cycle over the grouped images (temporal slices, or whatever the user has decided).
                            // Harvest the time course or any other voxel-specific numbers.
                            samples_1D<double> channel_time_course;
                            channel_time_course.uncertainties_known_to_be_independent_and_random = true;
                            for(auto & img_it : selected_img_its){
                                //Collect the datum of voxels and nearby voxels for an average.
                                std::list<double> in_pixs;
                                const auto boxr = 0;
                                const auto min_datum = 1;
    
                                for(auto lrow = (row-boxr); lrow <= (row+boxr); ++lrow){
                                    for(auto lcol = (col-boxr); lcol <= (col+boxr); ++lcol){
                                        //Check if the coordinates are legal and in the ROI.
                                        if( !isininc(0,lrow,img_it->rows-1) || !isininc(0,lcol,img_it->columns-1) ) continue;
    
                                        //const auto boxpoint = first_img_it->spatial_location(row,col);  //For standard contours(?).
                                        //const auto neighbourpoint = vec3<double>(lrow*1.0, lcol*1.0, SliceLocation*1.0);  //For the pixel integer contours.
                                        const auto neighbourpoint = first_img_it->position(lrow,lcol);
                                        auto ProjectedNeighbourPoint = BestFitPlane.Project_Onto_Plane_Orthogonally(neighbourpoint);
                                        if(!ProjectedContour.Is_Point_In_Polygon_Projected_Orthogonally(BestFitPlane,
                                                                                                        ProjectedNeighbourPoint,
                                                                                                        AlreadyProjected)) continue;
                                        const auto val = static_cast<double>(img_it->value(lrow, lcol, chan));
                                        in_pixs.push_back(val);
                                    }
                                }
                                auto dt = img_it->GetMetadataValueAs<double>("dt");
                                if(!dt) FUNCERR("Image is missing time metadata. Bailing");

                                const auto avg_val = Stats::Mean(in_pixs);
                                if(in_pixs.size() < min_datum) continue; //If contours are too narrow so that there is too few datum for meaningful results.
                                //const auto avg_val_sigma = std::sqrt(Stats::Unbiased_Var_Est(in_pixs))/std::sqrt(1.0*in_pixs.size());
                                //channel_time_course.push_back(dt.value(), 0.0, avg_val, avg_val_sigma, InhibitSort);
                                channel_time_course.push_back(dt.value(), 0.0, avg_val, 0.0, InhibitSort);
                            }
                            channel_time_course.stable_sort();
                            if(channel_time_course.empty()) continue;
   
                            //==============================================================================
                            //Fit the model.

                            //theAIF = &Carterial;
                            //theVIF = &Cvenous;
                            theROI = &channel_time_course;

                            const int dimen = 5;
                            //Fitting parameters:      k1A,  tauA,  k1V,  tauV,  k2.
                            double params[dimen] = {   1.0,   0.5,  1.0,   0.5,  1.0 };  //Arbitrarily chosen...

                            // U/L bounds:             k1A,  tauA,  k1V,  tauV,  k2.
                            double l_bnds[dimen] = {   0.0,  -5.0,  0.0,  -5.0,  0.0 };
                            double u_bnds[dimen] = {   1.0,   5.0,  1.0,   5.0,  1.0 };
                    
                            nlopt_opt opt;
                            //opt = nlopt_create(NLOPT_LN_COBYLA, dimen);
                            opt = nlopt_create(NLOPT_LN_BOBYQA, dimen);
                            //opt = nlopt_create(NLOPT_LN_SBPLX, dimen);
                    
                            //opt = nlopt_create(NLOPT_GN_DIRECT, dimen);
                            //opt = nlopt_create(NLOPT_GN_CRS2_LM, dimen);
                            //opt = nlopt_create(NLOPT_GN_ESCH, dimen);
                            //opt = nlopt_create(NLOPT_GN_ISRES, dimen);
                    
                            nlopt_set_lower_bounds(opt, l_bnds);
                            nlopt_set_upper_bounds(opt, u_bnds);
                    
                            nlopt_set_min_objective(opt, func_to_min, nullptr);
                            nlopt_set_xtol_rel(opt, 1.0E-4);
                   
                            double func_min;
                            if(nlopt_optimize(opt, params, &func_min) < 0){
                                FUNCWARN("NLOpt fail");
                                ++Minimization_Failure_Count;
                            }
                            nlopt_destroy(opt);

                    
                            const double k1A  = params[0];
                            const double tauA = params[1];
                            const double k2V  = params[2];
                            const double tauV = params[3];
                            const double k2   = params[4];

                            const auto LiverPerfusion = (k1A + k2V);

                            //==============================================================================
 
                            //Update the value.
                            const auto newval = static_cast<float>(LiverPerfusion);
                            working.reference(row, col, chan) = newval;
                            curr_min_pixel = std::min(curr_min_pixel, newval);
                            curr_max_pixel = std::max(curr_max_pixel, newval);

                            // ----------------------------------------------------------------------------
    
                        }//Loop over channels.
    
                    //If we're in the bounding box but not the ROI, do something.
                    }else{
                        //for(auto chan = 0; chan < first_img_it->channels; ++chan){
                        //    const auto curr_val = working.value(row, col, chan);
                        //    if(curr_val != 0) FUNCERR("There are overlapping ROI bboxes. This code currently cannot handle this. "
                        //                              "You will need to run the functor individually on the overlapping ROIs.");
                        //    working.reference(row, col, chan) = static_cast<float>(10);
                        //}
                    } // If is in ROI or ROI bbox.
                } //Loop over cols
            } //Loop over rows
        } //Loop over ROIs.
    } //Loop over contour_collections.

    //Swap the original image with the working image.
    *first_img_it = working;

    //Alter the first image's metadata to reflect that averaging has occurred. You might want to consider
    // a selective whitelist approach so that unique IDs are not duplicated accidentally.
    first_img_it->metadata["Description"] = "Liver Pharmacokinetic Model";


    //Specify a reasonable default window.
    if( (curr_min_pixel != std::numeric_limits<float>::max())
    &&  (curr_max_pixel != std::numeric_limits<float>::min()) ){
        const float WindowCenter = (curr_min_pixel/2.0) + (curr_max_pixel/2.0);
        //const float WindowWidth  = 2.0 + curr_max_pixel - curr_min_pixel;
        const float WindowWidth  = curr_max_pixel - curr_min_pixel;
        first_img_it->metadata["WindowValidFor"] = first_img_it->metadata["Description"];
        first_img_it->metadata["WindowCenter"]   = Xtostring(WindowCenter);
        first_img_it->metadata["WindowWidth"]    = Xtostring(WindowWidth);

        first_img_it->metadata["PixelMinMaxValidFor"] = first_img_it->metadata["Description"];
        first_img_it->metadata["PixelMin"]            = Xtostring(curr_min_pixel);
        first_img_it->metadata["PixelMax"]            = Xtostring(curr_max_pixel);
    }

    return true;
}

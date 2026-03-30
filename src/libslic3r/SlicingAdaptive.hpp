// Based on implementation by @platsch

#ifndef slic3r_SlicingAdaptive_hpp_
#define slic3r_SlicingAdaptive_hpp_

#include "Slicing.hpp"
#include "admesh/stl.h"

namespace Slic3r
{

class ModelVolume;

class SlicingAdaptive
{
public:
    void  clear();
    void  set_slicing_parameters(SlicingParameters params) { m_slicing_params = params; }
    void  prepare(const ModelObject &object);
    // Return next layer height starting from the last print_z, using a quality measure
    // (quality in range from 0 to 1, 0 - highest quality at low layer heights, 1 - lowest print quality at high layer heights).
    // The layer height curve shall be centered roughly around the default profile's layer height for quality 0.5.
	float next_layer_height(const float print_z, float quality, size_t &current_facet);
    float horizontal_facet_distance(float z);
    // Return next layer height based on overhang angle using formula: h = max_surface_dist * sin(angle)
    // Optimized sweep-line algorithm for large meshes.
    float next_layer_height_overhang(const float print_z, float max_surface_dist, float min_h, float max_h, size_t &current_facet);

	struct FaceZ {
		std::pair<float, float> z_span;
		// Cosine of the normal vector towards the Z axis (absolute value for existing algorithms).
		float					n_cos;
		// Sine of the normal vector towards the Z axis.
		float					n_sin;
		// 3D area of the triangle face, used as weight for tessellation-robust averaging.
		float					area;
	};

protected:
	SlicingParameters 		m_slicing_params;

	std::vector<FaceZ>		m_faces;
};

}; // namespace Slic3r

#endif /* slic3r_SlicingAdaptive_hpp_ */

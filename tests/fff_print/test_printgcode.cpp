#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <iostream>
#include <fstream>
#include <map>
#include <sstream>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

#include "test_data.hpp"

#include <algorithm>
#ifdef _WIN32
// catch.hpp pulls in <windows.h> with WIN32_LEAN_AND_MEAN, leaving the NLS API (LCTYPE,
// LCMapStringW, ...) undeclared for boost::regex v5 w32 traits. Use the portable traits instead.
#define BOOST_REGEX_NO_W32
#endif
#include <boost/regex.hpp>

using namespace Slic3r;
using namespace Slic3r::Test;

boost::regex perimeters_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; perimeter");
boost::regex infill_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; infill");
boost::regex skirt_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; skirt");

SCENARIO( "PrintGCode basic functionality", "[PrintGCode]") {
    GIVEN("A default configuration and a print test object") {
        WHEN("the output is executed with no support material") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, {
                { "layer_height",					0.2 },
                { "first_layer_height",				0.2 },
                { "first_layer_extrusion_width",	0 },
                { "gcode_comments",					true },
                { "start_gcode",					"" }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains slic3r version") {
                REQUIRE(gcode.find(SLIC3R_VERSION) != std::string::npos);
            }
            //THEN("Exported text contains git commit id") {
            //    REQUIRE(gcode.find("; Git Commit") != std::string::npos);
            //    REQUIRE(gcode.find(SLIC3R_BUILD_ID) != std::string::npos);
            //}
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Exported text does not contain cooling markers (they were consumed)") {
                REQUIRE(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
            }

            THEN("GCode preamble is emitted.") {
                REQUIRE(gcode.find("G21 ; set units to millimeters") != std::string::npos);
            }

            THEN("Config options emitted for print config, default region config, default object config") {
                REQUIRE(gcode.find("; first_layer_temperature") != std::string::npos);
                REQUIRE(gcode.find("; layer_height") != std::string::npos);
                REQUIRE(gcode.find("; fill_density") != std::string::npos);
            }
            THEN("Infill is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
				boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("final Z height is 20mm") {
                double final_z = 0.0;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    final_z = std::max<double>(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                });
                REQUIRE(final_z == Approx(20.));
            }
        }
        WHEN("output is executed with complete objects and two differently-sized meshes") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20,TestMesh::cube_20x20x20}, print, model, {
                { "first_layer_extrusion_width",    0 },
                { "first_layer_height",             0.3 },
                { "layer_height",                   0.2 },
                { "support_material",               false },
                { "raft_layers",                    0 },
                { "complete_objects",               true },
                { "gcode_comments",                 true },
                { "between_objects_gcode",          "; between-object-gcode" }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Infill is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                boost::smatch has_match;
                REQUIRE(boost::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("Between-object-gcode is emitted.") {
                REQUIRE(gcode.find("; between-object-gcode") != std::string::npos);
            }
            THEN("final Z height is 20.1mm") {
                double final_z = 0.0;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                });
                REQUIRE(final_z == Approx(20.1));
            }
            THEN("Z height resets on object change") {
                double final_z = 0.0;
                bool reset = false;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z, &reset] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    if (final_z > 0 && std::abs(self.z() - 0.3) < 0.01 ) { // saw higher Z before this, now it's lower
                        reset = true;
                    } else {
                        final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                    }
                });
                REQUIRE(reset == true);
            }
            THEN("Shorter object is printed before taller object.") {
                double final_z = 0.0;
                bool reset = false;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z, &reset] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    if (final_z > 0 && std::abs(self.z() - 0.3) < 0.01 ) { 
                        reset = (final_z > 20.0);
                    } else {
                        final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                    }
                });
                REQUIRE(reset == true);
            }
        }
        WHEN("the output is executed with support material") {
            std::string gcode = ::Test::slice({TestMesh::cube_20x20x20}, {
                { "first_layer_extrusion_width",    0 },
                { "support_material",               true },
                { "raft_layers",                    3 },
                { "gcode_comments",                 true }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Raft is emitted.") {
                REQUIRE(gcode.find("; raft") != std::string::npos);
            }
        }
        WHEN("the output is executed with a separate first layer extrusion width") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "first_layer_extrusion_width", "0.5" }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") != std::string::npos);
            }
        }
        WHEN("Cooling is enabled and the fan is disabled.") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
				{ "cooling",                    true },
                { "disable_fan_first_layers",   5 }
                });
            THEN("GCode to disable fan is emitted."){
                REQUIRE(gcode.find("M107") != std::string::npos);
            }
        }
        WHEN("end_gcode exists with layer_num and layer_z") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
				{ "end_gcode",              "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
                { "layer_height",           0.1 },
                { "first_layer_height",     0.1 }
                });
            THEN("layer_num and layer_z are processed in the end gcode") {
                REQUIRE(gcode.find("; Layer_num 199") != std::string::npos);
                REQUIRE(gcode.find("; Layer_z 20") != std::string::npos);
            }
        }
        WHEN("current_extruder exists in start_gcode") {
            {
				std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
					{ "start_gcode", "; Extruder [current_extruder]" }
                });
                THEN("current_extruder is processed in the start gcode and set for first extruder") {
                    REQUIRE(gcode.find("; Extruder 0") != std::string::npos);
                }
            }
			{
                DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
                config.set_num_extruders(4);
                config.set_deserialize_strict({
                    { "start_gcode",                    "; Extruder [current_extruder]" },
                    { "infill_extruder",                2 },
                    { "solid_infill_extruder",          2 },
                    { "perimeter_extruder",             2 },
                    { "support_material_extruder",      2 },
                    { "support_material_interface_extruder", 2 }
                });
                std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                THEN("current_extruder is processed in the start gcode and set for second extruder") {
                    REQUIRE(gcode.find("; Extruder 1") != std::string::npos);
                }
            }
        }

        WHEN("layer_num represents the layer's index from z=0") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, {
				{ "complete_objects",               true },
                { "gcode_comments",                 true },
                { "layer_gcode",                    ";Layer:[layer_num] ([layer_z] mm)" },
                { "layer_height",                   0.1 },
                { "first_layer_height",             0.1 }
                });
			// End of the 1st object.
            std::string token = ";Layer:199 ";
			size_t pos = gcode.find(token);
			THEN("First and second object last layer is emitted") {
				// First object
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				double z = 0;
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE(z == Approx(20.));
				// Second object
				pos = gcode.find(";Layer:399 ", pos);
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE(z == Approx(20.));
			}
        }
    }
}

// single_path_mode regression guard (end-to-end, at the G-code level). The polyline-level test in
// test_fill.cpp cannot see emission-stage bugs (e.g. the can_reverse bug that started the infill at the
// wrong end, 200-300 mm from the wall). It loads a REAL Ginger project 3mf (model + complete embedded
// pellet config, single_path_mode on) - so no synthetic/incomplete config - and asserts the invariant
// on the emitted G-code: zero travel between two infill extrusions of one connected region, plus a loose
// wall->infill bound. Tag [.] = hidden (run explicitly: fff_print_tests "[SinglePath]") because it needs
// the local fixture and slices a real part.
SCENARIO("Single path: zero travel inside connected infill", "[.][SinglePath]") {
    const std::string path = std::string(TEST_DATA_DIR) + "/single_path_guard.3mf";
    if (! boost::filesystem::exists(path)) {
        WARN("fixture tests/data/single_path_guard.3mf not present; skipping single-path travel guard");
        return;
    }
    GIVEN("the real single_path_mode part loaded from a 3mf") {
        DynamicPrintConfig        config;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        PlateDataPtrs             plate_data;
        std::vector<Preset*>      project_presets;
        // Must include LoadModel (=geometry) and LoadConfig; AddDefaultInstances alone loads neither,
        // leaving model.objects empty. Pass plate_data so the project's plate/objects are reconstructed.
        LoadStrategy strategy = LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel | LoadStrategy::LoadConfig;
        Model model = Model::read_from_file(path, &config, &ctx, strategy, &plate_data, &project_presets);
        REQUIRE(! model.objects.empty());

        config.set_key_value("gcode_comments", new ConfigOptionBool(true)); // role comments for classification
        config.normalize_fdm();

        Print print;
        print.apply(model, config);
        std::string gcode = Slic3r::Test::gcode(print); // process() + export (passes a valid result)
        REQUIRE(! gcode.empty());

        // Classify each move by the per-feature ;TYPE: role tag (emitted by _extrude when the role
        // changes), NOT by the inline description comment, which is "infill" for sparse AND every
        // solid/top/bottom surface. This mirrors build\analyze_feature_travels.ps1 and lets us check
        // the wall -> top/bottom/solid coincidence that the extended anchor now targets.
        struct Acc { size_t count = 0; double sum = 0.; double max = 0.;
                     void add(double mm) { ++count; sum += mm; max = std::max(max, mm); } };
        std::map<std::string, Acc> by_transition;
        std::string cur_type;            // role from the most recent ;TYPE: line
        std::string last_extruded_type;  // role of the previous extruding move
        bool        pending_travel    = false;
        double      pending_travel_mm = 0.;

        GCodeReader reader;
        reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
            const std::string_view c = line.comment();
            if (c.rfind("TYPE:", 0) == 0) {                             // a ";TYPE:<role>" line
                cur_type = std::string(c.substr(5));
                return;
            }
            if (line.travel() && line.dist_XY(self) > 1.0f) {           // a real travel move (> 1 mm)
                pending_travel    = true;
                pending_travel_mm = line.dist_XY(self);
            } else if (line.extruding(self)) {
                // The wall->infill travel (if any) is emitted before the new ;TYPE:, so cur_type here
                // already names the destination feature and last_extruded_type names the source.
                if (pending_travel && ! last_extruded_type.empty())
                    by_transition[last_extruded_type + " -> " + cur_type].add(pending_travel_mm);
                pending_travel     = false;
                last_extruded_type = cur_type;
            }
        });

        auto is_wall  = [](const std::string& t) { return t == "Inner wall" || t == "Outer wall"; };
        auto is_solid = [](const std::string& t) { return t == "Top surface" || t == "Bottom surface" || t == "Internal solid infill"; };
        Acc  wall_to_sparse, wall_to_solid;
        for (const auto& kv : by_transition) {
            const auto  sep  = kv.first.find(" -> ");
            const std::string from = kv.first.substr(0, sep);
            const std::string to   = kv.first.substr(sep + 4);
            if (is_wall(from) && to == "Sparse infill")
                { wall_to_sparse.count += kv.second.count; wall_to_sparse.sum += kv.second.sum; wall_to_sparse.max = std::max(wall_to_sparse.max, kv.second.max); }
            if (is_wall(from) && is_solid(to))
                { wall_to_solid.count  += kv.second.count; wall_to_solid.sum  += kv.second.sum;  wall_to_solid.max  = std::max(wall_to_solid.max,  kv.second.max); }
        }

        // Always-visible breakdown (WARN prints regardless of pass/fail) - read this for the real numbers.
        // NOTE: same-role infill->infill travels here are the unavoidable inter-island / inter-layer hops
        // (this multi-island part groups same-role features under one ;TYPE: block), NOT single-path breaks
        // inside one connected region - gcode alone cannot tell those apart without region info. Intra-region
        // single-path integrity (no break, no retraced segment) is asserted by the polyline-level tests in
        // test_fill.cpp; here we guard the wall->infill coincidence that the gcode-stage anchor controls.
        {
            std::ostringstream os;
            os << "single_path travel breakdown (travels > 1mm, by ;TYPE: transition):\n";
            for (const auto& kv : by_transition)
                os << "  " << kv.first << ": count=" << kv.second.count
                   << " total=" << (long) (kv.second.sum + 0.5) << "mm max=" << kv.second.max << "mm\n";
            WARN(os.str());
        }

        THEN("the wall to sparse-infill transition is not a gross jump (can_reverse-style guard)") {
            CAPTURE(wall_to_sparse.max);
            CHECK(wall_to_sparse.max < 50.0);
        }
        THEN("the wall to top/bottom/solid transition coincides with the monotonic start (extended anchor)") {
            // With the extended anchor the inner wall ends on the monotonic start, so these travels
            // should be absent or tiny. Gross-divergence guard here; the WARN breakdown shows the real value.
            CAPTURE(wall_to_solid.count, wall_to_solid.sum, wall_to_solid.max);
            CHECK(wall_to_solid.max < 50.0);
        }
    }
}

// Exploratory guard for LIGHTNING infill single-path with fill_multiline=2. The UI gate
// (have_connectable_infill_pattern) excludes lightning, but Fill.cpp gates connect_polygons only on
// the role (erInternalInfill) - lightning IS erInternalInfill and FillLightning already routes through
// chain_or_connect_infill - so we can exercise the connector on the lightning TREE topology headless by
// overriding the loaded config, no GUI build needed. Question being answered: does the single-path
// connector (built for scanlines) produce a clean single path on a widened lightning tree, or does it
// leave many internal travels? Reports the per-;TYPE: travel breakdown for inspection.
SCENARIO("Single path: lightning fill_multiline=2", "[.][SinglePathLightning]") {
    const std::string path = std::string(TEST_DATA_DIR) + "/single_path_guard.3mf";
    if (! boost::filesystem::exists(path)) {
        WARN("fixture tests/data/single_path_guard.3mf not present; skipping lightning single-path probe");
        return;
    }
    GIVEN("the real part re-configured to lightning + fill_multiline=2 + single_path_mode") {
        DynamicPrintConfig        config;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
        PlateDataPtrs             plate_data;
        std::vector<Preset*>      project_presets;
        LoadStrategy strategy = LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel | LoadStrategy::LoadConfig;
        Model model = Model::read_from_file(path, &config, &ctx, strategy, &plate_data, &project_presets);
        REQUIRE(! model.objects.empty());

        config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipLightning));
        config.set_key_value("fill_multiline",        new ConfigOptionInt(2));
        config.set_key_value("single_path_mode",      new ConfigOptionBool(true));
        config.set_key_value("gcode_comments",        new ConfigOptionBool(true));
        config.normalize_fdm();

        Print print;
        print.apply(model, config);
        std::string gcode = Slic3r::Test::gcode(print);
        REQUIRE(! gcode.empty());
        if (const char* dump = std::getenv("SINGLEPATH_DUMP")) { std::ofstream(dump, std::ios::binary) << gcode; }

        struct Acc { size_t count = 0; double sum = 0.; double max = 0.;
                     void add(double mm) { ++count; sum += mm; max = std::max(max, mm); } };
        std::map<std::string, Acc> by_transition;
        std::string cur_type, last_extruded_type;
        bool   pending_travel    = false;
        double pending_travel_mm = 0.;
        // Out-and-back doubled-extrusion detector inside Sparse infill (= lightning here), mirrors
        // build\find_spikes.ps1: two consecutive extrusion legs >= 5mm whose directions are nearly
        // opposite (> 175 deg) means the path retraces over (or right next to) what it just laid down.
        // This is the critical "no extrusion over already-extruded lines" property (priority #2), most at
        // risk on the lightning TREE where Euler augmentation could materialize retraced segments.
        bool   have_prev_dir = false;
        double prev_dx = 0., prev_dy = 0., prev_len = 0.;
        size_t spikes = 0;
        GCodeReader reader;
        reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
            const std::string_view c = line.comment();
            if (c.rfind("TYPE:", 0) == 0) { cur_type = std::string(c.substr(5)); have_prev_dir = false; return; }
            if (line.travel() && line.dist_XY(self) > 1.0f) {
                pending_travel = true; pending_travel_mm = line.dist_XY(self);
                have_prev_dir = false;                                  // a travel breaks the leg chain
            } else if (line.extruding(self)) {
                if (pending_travel && ! last_extruded_type.empty())
                    by_transition[last_extruded_type + " -> " + cur_type].add(pending_travel_mm);
                pending_travel = false; last_extruded_type = cur_type;
                const bool is_infill_type = cur_type == "Sparse infill" || cur_type == "Internal solid infill" ||
                                            cur_type == "Top surface" || cur_type == "Bottom surface";
                if (is_infill_type) {                                  // lightning sparse + connected solid/top/bottom
                    const double dx = line.new_X(self) - self.x(), dy = line.new_Y(self) - self.y();
                    const double len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.001) {
                        if (have_prev_dir && len >= 5.0 && prev_len >= 5.0) {
                            const double dot = (dx * prev_dx + dy * prev_dy) / (len * prev_len);
                            if (dot < -0.996) ++ spikes;               // > 174.9 deg reversal
                        }
                        prev_dx = dx; prev_dy = dy; prev_len = len; have_prev_dir = true;
                    }
                } else have_prev_dir = false;
            }
        });

        Acc wall_to_sparse, sparse_internal;
        for (const auto& kv : by_transition) {
            const auto sep = kv.first.find(" -> ");
            const std::string from = kv.first.substr(0, sep), to = kv.first.substr(sep + 4);
            if ((from == "Inner wall" || from == "Outer wall") && to == "Sparse infill")
                { wall_to_sparse.count += kv.second.count; wall_to_sparse.sum += kv.second.sum; wall_to_sparse.max = std::max(wall_to_sparse.max, kv.second.max); }
            if (from == "Sparse infill" && to == "Sparse infill")
                { sparse_internal.count += kv.second.count; sparse_internal.sum += kv.second.sum; sparse_internal.max = std::max(sparse_internal.max, kv.second.max); }
        }
        {
            std::ostringstream os;
            os << "LIGHTNING ml=2 single_path travel breakdown (travels > 1mm, by ;TYPE: transition):\n";
            for (const auto& kv : by_transition)
                os << "  " << kv.first << ": count=" << kv.second.count
                   << " total=" << (long) (kv.second.sum + 0.5) << "mm max=" << kv.second.max << "mm\n";
            WARN(os.str());
        }
        THEN("report wall->infill and sparse-internal travel (inspection, loose guards)") {
            CAPTURE(wall_to_sparse.count, wall_to_sparse.sum, wall_to_sparse.max);
            CAPTURE(sparse_internal.count, sparse_internal.sum, sparse_internal.max);
            CHECK(wall_to_sparse.max < 50.0);
        }
        THEN("lightning single-path does not retrace over its own lines (no out-and-back doubled extrusion)") {
            CAPTURE(spikes);
            CHECK(spikes == 0);
        }
    }
}

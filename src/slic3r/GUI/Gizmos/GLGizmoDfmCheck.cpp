#include "GLGizmoDfmCheck.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <GL/glew.h>

#include <algorithm>

#include <imgui/imgui.h>

namespace Slic3r::GUI {

// Flat colors for the thickness categories: 1x/2x nozzle are hard physical limits.
static const ColorRGBA THIN_CRITICAL_COLOR = {0.85f, 0.10f, 0.10f, 1.0f};
static const ColorRGBA THIN_WARNING_COLOR  = {0.95f, 0.55f, 0.05f, 1.0f};

// Overhang severity is graded by the lean angle (1 degree resolution): the gradients run
// over the FULL 0..90 range — the color IS the angle, there is no threshold to tune.
// Pale warm -> dark red for external, pale cool -> dark blue for internal, so the two
// families stay distinguishable while near-vertical walls stay close to the neutral gray.
static ColorRGBA overhang_gradient_color(int deg, bool external)
{
    const float t = std::clamp(float(deg) / 90.f, 0.f, 1.f);
    return external ? lerp(ColorRGBA(0.93f, 0.89f, 0.72f, 1.0f), ColorRGBA(0.75f, 0.03f, 0.03f, 1.0f), t) :
                      lerp(ColorRGBA(0.72f, 0.90f, 0.90f, 1.0f), ColorRGBA(0.05f, 0.15f, 0.65f, 1.0f), t);
}

// Extend call after only when the DfmCheck gizmo is still alive/active.
static void call_after_if_active(std::function<void()> fn, GUI_App *app = &wxGetApp())
{
    if (app == nullptr)
        return;
    app->CallAfter([fn, app]() {
        const Plater *plater = app->plater();
        if (plater == nullptr)
            return;
        const GLCanvas3D *canvas = plater->canvas3D();
        if (canvas == nullptr)
            return;
        const GLGizmosManager &mng = canvas->get_gizmos_manager();
        if (mng.get_current_type() != GLGizmosManager::DfmCheck)
            return;
        fn();
    });
}

static double active_nozzle_diameter()
{
    const auto *nozzle_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config
                                 .option<ConfigOptionFloats>("nozzle_diameter");
    return (nozzle_opt != nullptr && !nozzle_opt->values.empty()) ? nozzle_opt->get_at(0) : 3.;
}

GLGizmoDfmCheck::GLGizmoDfmCheck(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

GLGizmoDfmCheck::~GLGizmoDfmCheck()
{
    stop_worker_thread_request();
    if (m_worker.joinable())
        m_worker.join();
}

std::string GLGizmoDfmCheck::on_get_name() const
{
    return _u8L("Print check");
}

bool GLGizmoDfmCheck::on_is_activable() const
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF &&
           m_parent.get_selection().is_single_full_instance();
}

CommonGizmosDataID GLGizmoDfmCheck::on_get_requirements() const
{
    // InstancesHider shows only the analyzed instance and (through
    // GLCanvas3D::toggle_model_objects_visibility) renders it in neutral gray, so the
    // issue overlays stay readable whatever the filament color — same as the painter gizmos.
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::InstancesHider));
}

bool GLGizmoDfmCheck::AnalysisKey::operator==(const AnalysisKey &rhs) const
{
    if (object_id != rhs.object_id || instance_idx != rhs.instance_idx || volumes != rhs.volumes)
        return false;
    assert(linear_parts.size() == volumes.size());
    for (size_t i = 0; i < linear_parts.size(); ++i)
        if ((linear_parts[i] - rhs.linear_parts[i]).cwiseAbs().maxCoeff() > 1e-6)
            return false;
    return true;
}

std::optional<GLGizmoDfmCheck::AnalysisKey> GLGizmoDfmCheck::make_key(const Selection &selection)
{
    if (!selection.is_single_full_instance())
        return std::nullopt;
    const int object_idx   = selection.get_object_idx();
    const int instance_idx = selection.get_instance_idx();
    const Model &model     = wxGetApp().plater()->model();
    if (object_idx < 0 || object_idx >= int(model.objects.size()) || instance_idx < 0)
        return std::nullopt;
    const ModelObject *object = model.objects[object_idx];
    if (instance_idx >= int(object->instances.size()))
        return std::nullopt;

    AnalysisKey key;
    key.object_id    = object->id();
    key.instance_idx = instance_idx;
    const Transform3d instance_trafo = object->instances[instance_idx]->get_matrix();
    for (const ModelVolume *volume : object->volumes) {
        if (!volume->is_model_part())
            continue;
        key.volumes.emplace_back(volume->id(), volume->mesh().its.indices.size());
        // Only the linear part: translating the object never invalidates the analysis.
        key.linear_parts.emplace_back(Transform3d(instance_trafo * volume->get_matrix()).linear());
    }
    if (key.volumes.empty())
        return std::nullopt;
    return key;
}

const ModelObject *GLGizmoDfmCheck::find_analyzed_object() const
{
    if (!m_measure_key.has_value())
        return nullptr;
    for (const ModelObject *object : wxGetApp().plater()->model().objects)
        if (object->id() == m_measure_key->object_id)
            return object;
    return nullptr;
}

void GLGizmoDfmCheck::on_set_state()
{
    if (GLGizmoBase::m_state == GLGizmoBase::Off) {
        stop_worker_thread_request();
    } else if (GLGizmoBase::m_state == GLGizmoBase::On) {
        // Auto-analyze on open: the target user should not have to hunt for a button.
        m_thresholds.nozzle_diameter = active_nozzle_diameter();
        process();
        request_rerender(true);
    }
}

void GLGizmoDfmCheck::data_changed(bool is_serializing)
{
    if (GLGizmoBase::m_state != GLGizmoBase::On)
        return;
    std::optional<AnalysisKey> key = make_key(m_parent.get_selection());
    if (key.has_value() && (!m_measure_key.has_value() || *m_measure_key != *key))
        process();
}

void GLGizmoDfmCheck::stop_worker_thread_request()
{
    std::lock_guard lk(m_state_mutex);
    if (m_state.status == State::running)
        m_state.status = State::cancelling;
}

void GLGizmoDfmCheck::process()
{
    std::optional<AnalysisKey> key = make_key(m_parent.get_selection());
    if (!key.has_value())
        return;
    if (m_measure_key.has_value() && *m_measure_key == *key)
        return; // cached result is still valid

    {
        std::lock_guard lk(m_state_mutex);
        if (m_state.status == State::running) {
            if (m_state.key != *key)
                // Running for an outdated key: request cancellation. worker_finished()
                // will restart the analysis for the current key.
                m_state.status = State::cancelling;
            return;
        }
    }

    if (m_worker.joinable())
        m_worker.join();

    const Model &model = wxGetApp().plater()->model();
    const ModelObject *object = nullptr;
    for (const ModelObject *candidate : model.objects)
        if (candidate->id() == key->object_id) {
            object = candidate;
            break;
        }
    if (object == nullptr)
        return;

    // Snapshot the meshes on the UI thread: the worker must never touch the Model.
    auto snapshot = std::make_unique<DfmObjectSnapshot>(dfm_snapshot_object(*object, key->instance_idx));
    if (snapshot->world_its.indices.empty())
        return;

    {
        std::lock_guard lk(m_state_mutex);
        m_state.key      = *key;
        m_state.status   = State::running;
        m_state.progress = 0;
        m_state.result.reset();
    }

    m_worker = std::thread(
        [this](std::unique_ptr<DfmObjectSnapshot> snap) {
            auto cancel = [this]() {
                std::lock_guard lk(m_state_mutex);
                return m_state.status == State::cancelling;
            };
            auto progress = [this](int pct) {
                std::lock_guard lk(m_state_mutex);
                m_state.progress = pct;
                call_after_if_active([this]() { request_rerender(); });
            };
            auto result = std::make_unique<DfmMeasurement>(dfm_measure(snap->world_its, cancel, progress));
            result->volume_ranges   = std::move(snap->volume_ranges);
            result->skipped_volumes = snap->skipped_volumes;
            {
                std::lock_guard lk(m_state_mutex);
                if (m_state.status == State::running && !result->cancelled)
                    m_state.result = std::move(result);
                m_state.status = State::idle;
            }
            call_after_if_active([this]() { worker_finished(); });
        },
        std::move(snapshot));
}

// Called on the UI thread through CallAfter when the worker terminates.
void GLGizmoDfmCheck::worker_finished()
{
    {
        std::lock_guard lk(m_state_mutex);
        if (m_state.status == State::running)
            // Someone started the worker again before this callback ran.
            return;
    }
    if (m_worker.joinable())
        m_worker.join();
    if (GLGizmoBase::m_state == Off)
        return;
    {
        std::lock_guard lk(m_state_mutex);
        if (m_state.result) {
            m_measurement = std::move(*m_state.result);
            m_state.result.reset();
            m_measure_key        = m_state.key;
            m_reclassify_pending = true;
        }
    }
    // The selection may have moved on while we were measuring.
    std::optional<AnalysisKey> key = make_key(m_parent.get_selection());
    if (key.has_value() && (!m_measure_key.has_value() || *m_measure_key != *key))
        process();
    request_rerender(true);
}

void GLGizmoDfmCheck::reclassify()
{
    if (!m_measure_key.has_value() || m_measurement.facet_count() == 0)
        return;
    m_classification           = dfm_classify(m_measurement, m_thresholds);
    m_rebuild_overlays_pending = true;
}

void GLGizmoDfmCheck::rebuild_overlays()
{
    m_overlays.clear();
    const ModelObject *object = find_analyzed_object();
    if (object == nullptr || m_classification.facet_flags.size() != m_measurement.facet_count())
        return;

    for (const auto &[volume_id, range] : m_measurement.volume_ranges) {
        const ModelVolume *volume = nullptr;
        for (const ModelVolume *candidate : object->volumes)
            if (candidate->id() == volume_id) {
                volume = candidate;
                break;
            }
        if (volume == nullptr || volume->mesh().its.indices.size() != range.second - range.first)
            continue; // the mesh changed since the measurement; a recompute is on its way

        // The overlay is built from the LOCAL mesh (face order matches the world-space
        // snapshot by construction) and rendered with the volume's current world matrix,
        // so it follows the object while it is moved around the bed.
        const indexed_triangle_set &its = volume->mesh().its;
        auto overlay       = std::make_unique<VolumeOverlay>();
        overlay->volume_id = volume_id;

        // Default GLModel::Geometry format is already {Triangles, P3N3}.
        GLModel::Geometry thin_critical;
        GLModel::Geometry thin_warning;
        std::map<int, GLModel::Geometry> external_by_deg;
        std::map<int, GLModel::Geometry> internal_by_deg;

        for (size_t f = range.first; f < range.second; ++f) {
            const uint8_t flags = m_classification.facet_flags[f];
            if (flags == dfmNone)
                continue;
            // Each facet is displayed in a single category, most severe first; stats
            // still count every flag independently.
            GLModel::Geometry *geometry = nullptr;
            if (flags & dfmThinCritical)
                geometry = &thin_critical;
            else if (flags & dfmThinWarning)
                geometry = &thin_warning;
            else if (flags & dfmOverhangExternal)
                geometry = &external_by_deg[m_classification.overhang_deg[f]];
            else
                geometry = &internal_by_deg[m_classification.overhang_deg[f]];

            const size_t local = f - range.first;
            const Vec3f  normal = its_face_normal(its, int(local));
            const auto   base   = (unsigned int) geometry->vertices_count();
            for (int k = 0; k < 3; ++k)
                geometry->add_vertex(its.vertices[its.indices[local](k)], normal);
            geometry->add_triangle(base, base + 1, base + 2);
        }

        if (!thin_critical.is_empty()) {
            overlay->thin_critical.init_from(std::move(thin_critical));
            overlay->thin_critical.set_color(THIN_CRITICAL_COLOR);
        }
        if (!thin_warning.is_empty()) {
            overlay->thin_warning.init_from(std::move(thin_warning));
            overlay->thin_warning.set_color(THIN_WARNING_COLOR);
        }
        for (auto &[deg, geometry] : external_by_deg) {
            GLModel &model = overlay->external_by_deg[deg];
            model.init_from(std::move(geometry));
            model.set_color(overhang_gradient_color(deg, true));
        }
        for (auto &[deg, geometry] : internal_by_deg) {
            GLModel &model = overlay->internal_by_deg[deg];
            model.init_from(std::move(geometry));
            model.set_color(overhang_gradient_color(deg, false));
        }
        m_overlays.emplace_back(std::move(overlay));
    }
}

void GLGizmoDfmCheck::request_rerender(bool force)
{
    int64_t now = m_parent.timestamp_now();
    if (force || now > m_last_rerender_timestamp + 250) { // 250 ms
        set_dirty();
        m_parent.schedule_extra_frame(0);
        m_last_rerender_timestamp = now;
    }
}

void GLGizmoDfmCheck::on_render()
{
    if (m_overlays.empty() || !m_measure_key.has_value())
        return;

    auto *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    const Selection &selection = m_parent.get_selection();
    const Model     &model     = wxGetApp().plater()->model();

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    // The overlay triangles are coplanar with the object's own: pull them toward the
    // camera in depth to avoid z-fighting, at any model scale.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    // Mirrored instances flip the winding; keep both sides visible.
    glsafe(::glDisable(GL_CULL_FACE));

    const Camera      &camera      = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix = camera.get_view_matrix();

    for (unsigned int idx : selection.get_volume_idxs()) {
        const GLVolume *glvolume = selection.get_volume(idx);
        const GLVolume::CompositeID &cid = glvolume->composite_id;
        if (cid.instance_id != m_measure_key->instance_idx)
            continue;
        if (cid.object_id < 0 || cid.object_id >= int(model.objects.size()))
            continue;
        const ModelObject *object = model.objects[cid.object_id];
        if (object->id() != m_measure_key->object_id)
            continue;
        if (cid.volume_id < 0 || cid.volume_id >= int(object->volumes.size()))
            continue;
        const ObjectID volume_id = object->volumes[cid.volume_id]->id();

        const VolumeOverlay *overlay = nullptr;
        for (const auto &candidate : m_overlays)
            if (candidate->volume_id == volume_id) {
                overlay = candidate.get();
                break;
            }
        if (overlay == nullptr)
            continue;

        const Transform3d trafo = glvolume->world_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * trafo);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
                                            trafo.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        if (m_category_visible[0] && overlay->thin_critical.is_initialized())
            const_cast<GLModel &>(overlay->thin_critical).render();
        if (m_category_visible[1] && overlay->thin_warning.is_initialized())
            const_cast<GLModel &>(overlay->thin_warning).render();
        if (m_category_visible[2])
            for (const auto &[deg, bucket] : overlay->external_by_deg)
                const_cast<GLModel &>(bucket).render();
        if (m_category_visible[3])
            for (const auto &[deg, bucket] : overlay->internal_by_deg)
                const_cast<GLModel &>(bucket).render();
    }

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
}

// A small square color sample: flat color for the thickness categories, a miniature of
// the per-degree gradient for the overhang ones.
void GLGizmoDfmCheck::render_color_chip(size_t category)
{
    const float  size      = ImGui::GetFrameHeight() * 0.75f;
    const float  offset_y  = (ImGui::GetFrameHeight() - size) * 0.5f;
    const ImVec2 pos       = ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + offset_y);
    ImDrawList  *draw_list = ImGui::GetWindowDrawList();
    if (category <= 1) {
        const ColorRGBA &color = category == 0 ? THIN_CRITICAL_COLOR : THIN_WARNING_COLOR;
        draw_list->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size),
                                 ImGui::GetColorU32(ImVec4(color.r(), color.g(), color.b(), 1.f)), 2.f);
    } else {
        const bool external = category == 2;
        const int  segments = 6;
        for (int i = 0; i < segments; ++i) {
            const int deg = int(std::lround(90. * (i + 0.5) / segments));
            const ColorRGBA color = overhang_gradient_color(deg, external);
            draw_list->AddRectFilled(ImVec2(pos.x + size * i / segments, pos.y),
                                     ImVec2(pos.x + size * (i + 1) / segments, pos.y + size),
                                     ImGui::GetColorU32(ImVec4(color.r(), color.g(), color.b(), 1.f)));
        }
    }
    ImGui::Dummy(ImVec2(size, ImGui::GetFrameHeight()));
}

static std::string format_area(double area_mm2)
{
    const double cm2 = area_mm2 / 100.;
    return cm2 >= 1000. ? GUI::format("%.2f m²", cm2 / 10000.) : GUI::format("%.1f cm²", cm2);
}

void GLGizmoDfmCheck::render_issue_row(size_t category, const wxString &title, const wxString &explanation,
                                       float stats_col, float wrap_width)
{
    const DfmCategoryStats &stats = m_classification.stats[category];
    ImGui::PushID(int(category));

    ImGui::AlignTextToFramePadding();
    render_color_chip(category);
    ImGui::SameLine();
    if (m_imgui->bbl_checkbox(title, m_category_visible[category]))
        request_rerender(true);

    ImGui::SameLine(stats_col);
    if (stats.facets == 0) {
        m_imgui->text_colored(ImVec4(0.30f, 0.75f, 0.30f, 1.00f), _u8L("none"));
    } else {
        std::string line = format_area(stats.area);
        if (category >= 2)
            line += GUI::format(" · %1%°", stats.max_overhang_deg);
        m_imgui->text_colored(ImVec4(0.85f, 0.60f, 0.20f, 1.00f), line);
    }

    if (stats.facets > 0) {
        const float indent = ImGui::GetFrameHeight() * 0.75f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::Indent(indent);
        if (ImGui::TreeNodeEx(_u8L("Why & how to fix").c_str(), ImGuiTreeNodeFlags_None)) {
            // text_wrapped wraps at cursor.x + width: subtract the tree/row indent so the
            // text ends at the same right edge as the top-level rows and cannot widen the
            // width-pinned window.
            m_imgui->text_wrapped(explanation,
                wrap_width + ImGui::GetStyle().WindowPadding.x - ImGui::GetCursorPosX());
            ImGui::TreePop();
        }
        ImGui::Unindent(indent);
    }
    ImGui::PopID();
}

void GLGizmoDfmCheck::on_render_input_window(float x, float y, float bottom_limit)
{
    // The nozzle diameter follows the active printer preset live; it only affects the
    // cheap classify pass, never the raycast measurement.
    const double nozzle = active_nozzle_diameter();
    if (std::abs(nozzle - m_thresholds.nozzle_diameter) > EPSILON) {
        m_thresholds.nozzle_diameter = nozzle;
        m_reclassify_pending         = true;
    }

    if (m_reclassify_pending) {
        m_reclassify_pending = false;
        reclassify();
    }
    if (m_rebuild_overlays_pending) {
        m_rebuild_overlays_pending = false;
        rebuild_overlays();
    }

    bool is_running  = false;
    int  progress    = 0;
    {
        std::lock_guard lk(m_state_mutex);
        is_running = m_state.status != State::idle;
        progress   = m_state.progress;
    }

    // Clamp with the height measured INSIDE the window on the previous frame:
    // ImGui::GetWindowHeight() here would report whatever window happens to be current.
    y = std::min(y, bottom_limit - m_last_input_window_height);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    const float currt_scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(currt_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0 * currt_scale, 5.0 * currt_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * currt_scale);

    // Layout constants: issue title left, area/angle stats right. Computed BEFORE Begin
    // so the window WIDTH can be pinned: with AlwaysAutoResize alone, expanding a
    // "Why & how to fix" node widens the window, and the x clamp in
    // GizmoImguiSetNextWIndowPos (based on the previous frame's width) then makes the
    // whole panel jump left/right on every expand/collapse.
    const float space_size = m_imgui->get_style_scaling() * 8;
    std::vector<wxString> titles = {_L("Too thin — unprintable"), _L("Too thin for a wall loop"),
                                    _L("Overhang (outward)"), _L("Overhang (inward, over infill)")};
    const float stats_col    = m_imgui->find_widest_text(titles) + 2.f * ImGui::GetFrameHeight() + 3.f * space_size;
    const float panel_width  = stats_col + m_imgui->scaled(9.0f);
    const float window_width = panel_width + 2.f * ImGui::GetStyle().WindowPadding.x;
    ImGui::SetNextWindowSizeConstraints(ImVec2(window_width, 0.f), ImVec2(window_width, FLT_MAX));

    GizmoImguiBegin(get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    m_imgui->text_wrapped(GUI::format(_L("Nozzle %1% mm: walls need %2% mm to print and %3% mm for a wall loop."),
                                      GUI::format("%.2f", nozzle),
                                      GUI::format("%.2f", nozzle * m_thresholds.thin_critical_factor),
                                      GUI::format("%.2f", nozzle * m_thresholds.thin_warning_factor)),
                          panel_width);
    ImGui::Separator();

    if (is_running) {
        m_imgui->text(_u8L("Analyzing the model..."));
        ImGui::ProgressBar(float(progress) / 100.f, ImVec2(panel_width - ImGui::GetStyle().WindowPadding.x, 0.f));
        if (m_imgui->button(_L("Cancel")))
            stop_worker_thread_request();
    } else if (!m_measure_key.has_value()) {
        m_imgui->text_wrapped(_L("Select a single object to analyze its printability."), panel_width);
    } else {
        // The user may have changed something while the gizmo was closed.
        std::optional<AnalysisKey> key = make_key(m_parent.get_selection());
        if (key.has_value() && *key != *m_measure_key) {
            m_imgui->text_colored(ImVec4(0.85f, 0.60f, 0.20f, 1.00f), _u8L("The model changed."));
            ImGui::SameLine();
            if (m_imgui->button(_L("Recompute")))
                process();
            ImGui::Separator();
        }

        render_issue_row(0, titles[0],
            GUI::format_wxstr(_L("These walls are thinner than the nozzle (%1% mm). The printer cannot make "
                 "a bead this small, so these areas will come out missing or broken.\n"
                 "Fix: thicken the walls to at least twice the nozzle diameter, or scale the model up."),
                GUI::format("%.2f", nozzle)),
            stats_col, panel_width);
        render_issue_row(1, titles[1],
            GUI::format_wxstr(_L("These walls are thinner than two nozzle diameters (%1% mm). A printed wall "
                 "needs an outgoing and a returning bead, and both do not fit: the slicer will "
                 "leave a gap or drop to a single fragile bead.\n"
                 "Fix: design wall thicknesses as a multiple of the bead width (2x the nozzle or more)."),
                GUI::format("%.2f", 2. * nozzle)),
            stats_col, panel_width);
        render_issue_row(2, titles[2],
            _L("These surfaces face downward over empty space, and heavy pellet beads sag "
               "when there is no material below them. The color IS the surface angle: from "
               "pale (near vertical, 0°) to dark red (horizontal, 90°).\n"
               "Fix: reduce the angle (45° chamfers instead of flat ledges), rotate the part, "
               "or split the model and print it in pieces."),
            stats_col, panel_width);
        render_issue_row(3, titles[3],
            _L("These surfaces face upward: on the layers above, the wall bead rests on "
               "sparse infill, which cannot carry a heavy pellet bead — the wall can collapse "
               "inward. A flat top is the worst case of this (90°): the whole surface rests on "
               "infill. Slicers made for desktop printers ignore this problem completely! The "
               "color IS the surface angle: from pale (near vertical, 0°) to dark blue (90°).\n"
               "Fix: reduce the lean, use denser or solid infill under tops, or thicken the wall."),
            stats_col, panel_width);

        ImGui::Separator();
        m_imgui->text_wrapped(_L("Overhang colors grade the surface angle from vertical (pale, 0°) "
                                 "to horizontal (dark, 90°) — there is nothing to configure."),
                              panel_width);

        if (m_measurement.skipped_volumes > 0) {
            ImGui::Separator();
            m_imgui->text_wrapped(_L("Negative parts and modifiers are ignored by this analysis."), panel_width);
        }
        if (m_measurement.facet_count() > 0 &&
            m_measurement.ray_miss_count * 50 > m_measurement.facet_count()) { // > 2%
            ImGui::Separator();
            m_imgui->text_wrapped(_L("This model has holes or errors, so thickness results may be "
                                     "incomplete. Consider repairing the model first."), panel_width);
        }
    }

    // AlwaysAutoResize applies a content change only on the NEXT ImGui frame; without an
    // extra frame the window keeps the stale size (a big empty area after collapsing a
    // "Why & how to fix" node) until some other event triggers a render.
    m_last_input_window_height = ImGui::GetWindowHeight();
    const float content_height = ImGui::GetCursorPosY();
    if (std::abs(content_height - m_last_content_height) > 0.5f) {
        m_last_content_height = content_height;
        request_rerender(true);
    }

    GizmoImguiEnd();
    ImGui::PopStyleVar(2);
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI

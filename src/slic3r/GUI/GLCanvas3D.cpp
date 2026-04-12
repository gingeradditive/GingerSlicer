#include "libslic3r/libslic3r.h"
#include "GLCanvas3D.hpp"

#include <igl/unproject.h>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Technologies.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "3DScene.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "GLShader.hpp"
#include "GUI.hpp"
#include "Tab.hpp"
#include "GUI_Preview.hpp"
#include "OpenGLManager.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Colors.hpp"
#include "Mouse3DController.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "format.hpp"
#include "DailyTips.hpp"

#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <slic3r/GUI/GUI_Utils.hpp>

#if ENABLE_RETINA_GL
#include "slic3r/Utils/RetinaHelper.hpp"
#endif

#include <GL/glew.h>

#include <wx/glcanvas.h>
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/tooltip.h>
#include <wx/debug.h>
#include <wx/fontutil.h>
// Print now includes tbb, and tbb includes Windows. This breaks compilation of wxWidgets if included before wx.
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"

#include "wxExtensions.hpp"

#include <tbb/parallel_for.h>
#include <tbb/spin_mutex.h>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <iostream>
#include <float.h>
#include <algorithm>
#include <cmath>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include <imguizmo/ImGuizmo.h>

static constexpr const float TRACKBALLSIZE = 0.8f;

static Slic3r::ColorRGBA DEFAULT_BG_LIGHT_COLOR      = { 0.906f, 0.906f, 0.906f, 1.0f };
static Slic3r::ColorRGBA ERROR_BG_LIGHT_COLOR        = { 0.753f, 0.192f, 0.039f, 1.0f };

void GLCanvas3D::update_render_colors()
{
    DEFAULT_BG_LIGHT_COLOR = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_3D_Background]);
}

void GLCanvas3D::load_render_colors()
{
    RenderColor::colors[RenderCol_3D_Background] = ImGuiWrapper::to_ImVec4(DEFAULT_BG_LIGHT_COLOR);
}

//static constexpr const float AXES_COLOR[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

// Number of floats
static constexpr const size_t MAX_VERTEX_BUFFER_SIZE     = 131072 * 6; // 3.15MB

namespace Slic3r {
namespace GUI {

#ifdef __WXGTK3__
// wxGTK3 seems to simulate OSX behavior in regard to HiDPI scaling support.
RetinaHelper::RetinaHelper(wxWindow* window) : m_window(window), m_self(nullptr) {}
RetinaHelper::~RetinaHelper() {}
float RetinaHelper::get_scale_factor() { return float(m_window->GetContentScaleFactor()); }
#endif // __WXGTK3__

// Fixed the collision between BuildVolume_Type::Convex and macro Convex defined inside /usr/include/X11/X.h that is included by WxWidgets 3.0.
#if defined(__linux__) && defined(Convex)
#undef Convex
#endif

GLCanvas3D::LayersEditing::~LayersEditing()
{
    if (m_z_texture_id != 0) {
        glsafe(::glDeleteTextures(1, &m_z_texture_id));
        m_z_texture_id = 0;
    }
    delete m_slicing_parameters;
}

const float GLCanvas3D::LayersEditing::THICKNESS_BAR_WIDTH = 70.0f;

void GLCanvas3D::LayersEditing::init()
{
    glsafe(::glGenTextures(1, (GLuint*)&m_z_texture_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
}

void GLCanvas3D::LayersEditing::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    delete m_slicing_parameters;
    m_slicing_parameters = nullptr;
    m_layers_texture.valid = false;
    m_layer_height_profile.clear();
}

void GLCanvas3D::LayersEditing::select_object(const Model& model, int object_id)
{
    const ModelObject* model_object_new = (object_id >= 0) ? model.objects[object_id] : nullptr;
    // Maximum height of an object changes when the object gets rotated or scaled.
    // Changing maximum height of an object will invalidate the layer heigth editing profile.
    // m_model_object->bounding_box() is cached, therefore it is cheap even if this method is called frequently.
    const float new_max_z = (model_object_new == nullptr) ? 0.0f : static_cast<float>(model_object_new->max_z());

    if (m_model_object != model_object_new || this->last_object_id != object_id || m_object_max_z != new_max_z ||
        (model_object_new != nullptr && m_model_object->id() != model_object_new->id())) {
        m_layer_height_profile.clear();
        delete m_slicing_parameters;
        m_slicing_parameters = nullptr;
        m_layers_texture.valid = false;
        this->last_object_id = object_id;
        m_model_object = model_object_new;
        m_object_max_z = new_max_z;
        item.name = "splitobjects";
        item.icon_filename = "split_objects.svg";
    }
}

bool GLCanvas3D::LayersEditing::is_allowed() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr && m_z_texture_id > 0;
}

bool GLCanvas3D::LayersEditing::is_enabled() const
{
    return m_enabled;
}

void GLCanvas3D::LayersEditing::set_enabled(bool enabled)
{
    m_enabled = is_allowed() && enabled;
}

float GLCanvas3D::LayersEditing::s_overlay_window_width;

void GLCanvas3D::LayersEditing::show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y)
{
    ImTextureID normal_id = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float caption_max = 0.f;
    for (auto caption_text : captions_texts) {
        caption_max = std::max(imgui.calc_text_size(caption_text.first).x, caption_max);
    }
    caption_max += GImGui->Style.WindowPadding.x + imgui.scaled(1);

	float  scale       = canvas.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max, &imgui](const wxString& caption, const wxString& text) {
            imgui.text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui.text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto& caption_text : captions_texts) draw_text_with_caption(caption_text.first, caption_text.second);

        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLCanvas3D::LayersEditing::render_variable_layer_height_dialog(const GLCanvas3D& canvas) {
    if (!m_enabled)
        return;

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const Size& cnv_size = canvas.get_canvas_size();
    float left_pos = canvas.m_main_toolbar.get_item("layersediting")->render_left_pos;
    const float x = (1 + left_pos) * cnv_size.get_width() / 2;
    imgui.set_next_window_pos(x, canvas.m_main_toolbar.get_height(), ImGuiCond_Always, 0.0f, 0.0f);

    imgui.push_toolbar_style(canvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * canvas.get_scale(), 4.0f * canvas.get_scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * canvas.get_scale(), 10.0f * canvas.get_scale()));
    imgui.begin(_L("Variable layer height"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    const float sliders_width = imgui.scaled(7.0f);
    const float input_box_width = 1.5 * imgui.get_slider_icon_size().x;

    if (imgui.button(_L("Adaptive")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), Event<float>(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, m_adaptive_quality));
    ImGui::SameLine();
    static float text_align = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    text_align = std::max(text_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(text_align);
    imgui.text(_L("Quality / Speed"));
    if (ImGui::IsItemHovered()) {
        //ImGui::BeginTooltip();
        //ImGui::TextUnformatted(_L("Higher print quality versus higher print speed.").ToUTF8());
        //ImGui::EndTooltip();
    }
    ImGui::SameLine();
    static float slider_align = ImGui::GetCursorPosX();
    ImGui::PushItemWidth(sliders_width);
    m_adaptive_quality = std::clamp(m_adaptive_quality, 0.0f, 1.f);
    slider_align = std::max(slider_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(slider_align);
    imgui.bbl_slider_float_style("##adaptive_slider", &m_adaptive_quality, 0.0f, 1.f, "%.2f");
    ImGui::SameLine();
    static float input_align = ImGui::GetCursorPosX();
    ImGui::PushItemWidth(input_box_width);
    input_align = std::max(input_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(input_align);
    ImGui::BBLDragFloat("##adaptive_input", &m_adaptive_quality, 0.05f, 0.0f, 0.0f, "%.2f");

    if (imgui.button(_L("Smooth")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), HeightProfileSmoothEvent(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, m_smooth_params));
    ImGui::SameLine();
    text_align = std::max(text_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(text_align);
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Radius"));
    ImGui::SameLine();
    slider_align = std::max(slider_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(slider_align);
    ImGui::PushItemWidth(sliders_width);
    int radius = (int)m_smooth_params.radius;
    int v_min = 1, v_max = 10;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.81f, 0.81f, 0.81f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    if(ImGui::BBLSliderScalar("##radius_slider", ImGuiDataType_S32, &radius, &v_min, &v_max)){
        radius = std::clamp(radius, 1, 10);
        m_smooth_params.radius = (unsigned int)radius;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::SameLine();
    input_align = std::max(input_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(input_align);
    ImGui::PushItemWidth(input_box_width);
    ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.00f, 0.59f, 0.53f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.00f, 0.59f, 0.53f, 0.00f));
    ImGui::BBLDragScalar("##radius_input", ImGuiDataType_S32, &radius, 1, &v_min, &v_max);
    ImGui::PopStyleColor(3);

    imgui.bbl_checkbox("##keep_min", m_smooth_params.keep_min);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Keep min"));

    ImGui::Separator();

    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + canvas.m_main_toolbar.get_height();
    std::map<wxString, wxString> captions_texts = {
        {_L("Left mouse button") + ":" , _L("Add detail")},
        {_L("Right mouse button") + ":", _L("Remove detail")},
        {_L("Shift+") + _L("Left mouse button") + ":", _L("Reset to base")},
        {_L("Shift+") + _L("Right mouse button") + ":", _L("Smoothing")},
        {_L("Mouse wheel:"), _L("Increase/decrease edit area")}
    };
    show_tooltip_information(canvas, captions_texts, x, get_cur_y);
    ImGui::SameLine();
    if (imgui.button(_L("Reset")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), SimpleEvent(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE));

    GLCanvas3D::LayersEditing::s_overlay_window_width = ImGui::GetWindowSize().x;
    imgui.end();
    ImGui::PopStyleVar(2);
    imgui.pop_toolbar_style();
}

void GLCanvas3D::LayersEditing::render_overlay(const GLCanvas3D& canvas)
{
    render_variable_layer_height_dialog(canvas);
    render_active_object_annotations(canvas);
    render_profile(canvas);
}

float GLCanvas3D::LayersEditing::get_cursor_z_relative(const GLCanvas3D& canvas)
{
    const Vec2d mouse_pos = canvas.get_local_mouse_position();
    const Rect& rect = get_bar_rect_screen(canvas);
    float x = (float)mouse_pos.x();
    float y = (float)mouse_pos.y();
    float t = rect.get_top();
    float b = rect.get_bottom();

    return (rect.get_left() <= x && x <= rect.get_right() && t <= y && y <= b) ?
        // Inside the bar.
        (b - y - 1.0f) / (b - t - 1.0f) :
        // Outside the bar.
        -1000.0f;
}

bool GLCanvas3D::LayersEditing::bar_rect_contains(const GLCanvas3D& canvas, float x, float y)
{
    const Rect& rect = get_bar_rect_screen(canvas);
    return rect.get_left() <= x && x <= rect.get_right() && rect.get_top() <= y && y <= rect.get_bottom();
}

Rect GLCanvas3D::LayersEditing::get_bar_rect_screen(const GLCanvas3D& canvas)
{
    const Size& cnv_size = canvas.get_canvas_size();
    float w = (float)cnv_size.get_width();
    float h = (float)cnv_size.get_height();

    return { w - thickness_bar_width(canvas), 0.0f, w, h };
}

bool GLCanvas3D::LayersEditing::is_initialized() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr;
}

std::string GLCanvas3D::LayersEditing::get_tooltip(const GLCanvas3D& canvas) const
{
    std::string ret;
    if (m_enabled && m_layer_height_profile.size() >= 4) {
        float z = get_cursor_z_relative(canvas);
        if (z != -1000.0f) {
            z *= m_object_max_z;

            float h = 0.0f;
            for (size_t i = m_layer_height_profile.size() - 2; i >= 2; i -= 2) {
                const float zi = static_cast<float>(m_layer_height_profile[i]);
                const float zi_1 = static_cast<float>(m_layer_height_profile[i - 2]);
                if (zi_1 <= z && z <= zi) {
                    float dz = zi - zi_1;
                    h = (dz != 0.0f) ? static_cast<float>(lerp(m_layer_height_profile[i - 1], m_layer_height_profile[i + 1], (z - zi_1) / dz)) :
                        static_cast<float>(m_layer_height_profile[i + 1]);
                    break;
                }
            }
            if (h > 0.0f)
                ret = wxString::Format("%.3f",h).ToStdString();
        }
    }
    return ret;
}

void GLCanvas3D::LayersEditing::render_active_object_annotations(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    const Size cnv_size = canvas.get_canvas_size();
    const float cnv_width  = (float)cnv_size.get_width();
    const float cnv_height = (float)cnv_size.get_height();
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;

    const float cnv_inv_width = 1.0f / cnv_width;

    GLShaderProgram* shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    shader->start_using();

    shader->set_uniform("z_to_texture_row", float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * m_object_max_z));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / (float)m_layers_texture.height);
    shader->set_uniform("z_cursor", m_object_max_z * this->get_cursor_z_relative(canvas));
    shader->set_uniform("z_cursor_band_width", band_width);
    shader->set_uniform("object_max_z", m_object_max_z);
    shader->set_uniform("view_model_matrix", Transform3d::Identity());
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    shader->set_uniform("view_normal_matrix", (Matrix3d)Matrix3d::Identity());

    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));

    // Render the color bar
    if (!m_profile.background.is_initialized() || m_profile.old_canvas_width != cnv_width) {
        m_profile.old_canvas_width = cnv_width;
        m_profile.background.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);

        // vertices
        const float l = 1.0f - 2.0f * THICKNESS_BAR_WIDTH * cnv_inv_width;
        const float r = 1.0f;
        const float t = 1.0f;
        const float b = -1.0f;
        init_data.add_vertex(Vec3f(l, b, 0.0f), Vec3f::UnitZ(), Vec2f(0.0f, 0.0f));
        init_data.add_vertex(Vec3f(r, b, 0.0f), Vec3f::UnitZ(), Vec2f(1.0f, 0.0f));
        init_data.add_vertex(Vec3f(r, t, 0.0f), Vec3f::UnitZ(), Vec2f(1.0f, 1.0f));
        init_data.add_vertex(Vec3f(l, t, 0.0f), Vec3f::UnitZ(), Vec2f(0.0f, 1.0f));

        // indices
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(2, 3, 0);

        m_profile.background.init_from(std::move(init_data));
    }

    m_profile.background.render();

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    shader->stop_using();
}

void GLCanvas3D::LayersEditing::render_profile(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    //FIXME show some kind of legend.

    if (!m_slicing_parameters)
        return;

    const Size cnv_size = canvas.get_canvas_size();
    const float cnv_width  = (float)cnv_size.get_width();
    const float cnv_height = (float)cnv_size.get_height();
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;

    // Make the vertical bar a bit wider so the layer height curve does not touch the edge of the bar region.
    const float scale_x = THICKNESS_BAR_WIDTH / float(1.12 * m_slicing_parameters->max_layer_height);
    const float scale_y = cnv_height / m_object_max_z;

    const float cnv_inv_width  = 1.0f / cnv_width;
    const float cnv_inv_height = 1.0f / cnv_height;

    // Baseline
    if (!m_profile.baseline.is_initialized() || m_profile.old_layer_height_profile != m_layer_height_profile) {
        m_profile.baseline.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P2};
        init_data.color = ColorRGBA::BLACK();
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        const float axis_x = 2.0f * ((cnv_width - THICKNESS_BAR_WIDTH + float(m_slicing_parameters->layer_height) * scale_x) * cnv_inv_width - 0.5f);
        init_data.add_vertex(Vec2f(axis_x, -1.0f));
        init_data.add_vertex(Vec2f(axis_x, 1.0f));

        // indices
        init_data.add_line(0, 1);

        m_profile.baseline.init_from(std::move(init_data));
    }

    if (!m_profile.profile.is_initialized() || m_profile.old_layer_height_profile != m_layer_height_profile) {
        m_profile.old_layer_height_profile = m_layer_height_profile;
        m_profile.profile.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P2 };
        init_data.color = ColorRGBA::BLUE();
        init_data.reserve_vertices(m_layer_height_profile.size() / 2);
        init_data.reserve_indices(m_layer_height_profile.size() / 2);

        // vertices + indices
        for (unsigned int i = 0; i < (unsigned int)m_layer_height_profile.size(); i += 2) {
            init_data.add_vertex(Vec2f(2.0f * ((cnv_width - THICKNESS_BAR_WIDTH + float(m_layer_height_profile[i + 1]) * scale_x) * cnv_inv_width - 0.5f),
                                       2.0f * (float(m_layer_height_profile[i]) * scale_y * cnv_inv_height - 0.5)));
            init_data.add_index(i / 2);
        }

        m_profile.profile.init_from(std::move(init_data));
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_profile.baseline.render();
        m_profile.profile.render();
        shader->stop_using();
    }
}

void GLCanvas3D::LayersEditing::render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes)
{
    assert(this->is_allowed());
    assert(this->last_object_id != -1);

    GLShaderProgram* current_shader = wxGetApp().get_current_shader();
    ScopeGuard guard([current_shader]() { if (current_shader != nullptr) current_shader->start_using(); });
    if (current_shader != nullptr)
        current_shader->stop_using();

    GLShaderProgram* shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    shader->start_using();

    generate_layer_height_texture();

    // Uniforms were resolved, go ahead using the layer editing shader.
    shader->set_uniform("z_to_texture_row", float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * float(m_object_max_z)));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / float(m_layers_texture.height));
    shader->set_uniform("z_cursor", float(m_object_max_z) * float(this->get_cursor_z_relative(canvas)));
    shader->set_uniform("z_cursor_band_width", float(this->band_width));

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // Initialize the layer height texture mapping.
    const GLsizei w = (GLsizei)m_layers_texture.width;
    const GLsizei h = (GLsizei)m_layers_texture.height;
    const GLsizei half_w = w / 2;
    const GLsizei half_h = h / 2;
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data()));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data() + m_layers_texture.width * m_layers_texture.height * 4));
    for (GLVolume* glvolume : volumes.volumes) {
        // Render the object using the layer editing shader and texture.
        if (!glvolume->is_active || glvolume->composite_id.object_id != this->last_object_id || glvolume->is_modifier)
            continue;

        shader->set_uniform("volume_world_matrix", glvolume->world_matrix());
        shader->set_uniform("object_max_z", 0.0f);
        const Transform3d& view_matrix = camera.get_view_matrix();
        const Transform3d model_matrix = glvolume->world_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        glvolume->render();
    }
    // Revert back to the previous shader.
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLCanvas3D::LayersEditing::adjust_layer_height_profile()
{
    this->update_slicing_parameters();
    PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile);
    Slic3r::adjust_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile, this->last_z, this->strength, this->band_width, this->last_action);
    m_layers_texture.valid = false;
}

void GLCanvas3D::LayersEditing::reset_layer_height_profile(GLCanvas3D & canvas)
{
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.clear();
    m_layer_height_profile.clear();
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
}

void GLCanvas3D::LayersEditing::adaptive_layer_height_profile(GLCanvas3D & canvas, float quality_factor)
{
    this->update_slicing_parameters();
    m_layer_height_profile = layer_height_profile_adaptive(*m_slicing_parameters, *m_model_object, quality_factor);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
}

void GLCanvas3D::LayersEditing::smooth_layer_height_profile(GLCanvas3D & canvas, const HeightProfileSmoothingParams & smoothing_params)
{
    this->update_slicing_parameters();
    m_layer_height_profile = smooth_height_profile(m_layer_height_profile, *m_slicing_parameters, smoothing_params);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
}

void GLCanvas3D::LayersEditing::generate_layer_height_texture()
{
    this->update_slicing_parameters();
    // Always try to update the layer height profile.
    bool update = !m_layers_texture.valid;
    if (PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile)) {
        // Initialized to the default value.
        update = true;
    }
    // Update if the layer height profile was changed, or when the texture is not valid.
    if (!update && !m_layers_texture.data.empty() && m_layers_texture.cells > 0)
        // Texture is valid, don't update.
        return;

    if (m_layers_texture.data.empty()) {
        m_layers_texture.width = 1024;
        m_layers_texture.height = 1024;
        m_layers_texture.levels = 2;
        m_layers_texture.data.assign(m_layers_texture.width * m_layers_texture.height * 5, 0);
    }

    bool level_of_detail_2nd_level = true;
    m_layers_texture.cells = Slic3r::generate_layer_height_texture(
        *m_slicing_parameters,
        Slic3r::generate_object_layers(*m_slicing_parameters, m_layer_height_profile, false),
        m_layers_texture.data.data(), m_layers_texture.height, m_layers_texture.width, level_of_detail_2nd_level);
    m_layers_texture.valid = true;
}

void GLCanvas3D::LayersEditing::accept_changes(GLCanvas3D & canvas)
{
    if (last_object_id >= 0) {
        wxGetApp().plater()->take_snapshot("Variable layer height - Manual edit");
        const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
        canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        wxGetApp().obj_list()->update_info_items(last_object_id);
    }
}

void GLCanvas3D::LayersEditing::update_slicing_parameters()
{
    if (m_slicing_parameters == nullptr) {
        m_slicing_parameters = new SlicingParameters();
        *m_slicing_parameters = PrintObject::slicing_parameters(*m_config, *m_model_object, m_object_max_z, m_shrinkage_compensation);
    }

}

float GLCanvas3D::LayersEditing::thickness_bar_width(const GLCanvas3D & canvas)
{
    return
#if ENABLE_RETINA_GL
        canvas.get_canvas_size().get_scale_factor()
#else
        canvas.get_wxglcanvas()->GetContentScaleFactor()
#endif
        * THICKNESS_BAR_WIDTH;
}

const Point GLCanvas3D::Mouse::Drag::Invalid_2D_Point(INT_MAX, INT_MAX);
const Vec3d GLCanvas3D::Mouse::Drag::Invalid_3D_Point(DBL_MAX, DBL_MAX, DBL_MAX);
const int GLCanvas3D::Mouse::Drag::MoveThresholdPx = 5;

GLCanvas3D::Mouse::Drag::Drag()
    : start_position_2D(Invalid_2D_Point)
    , start_position_3D(Invalid_3D_Point)
    , move_volume_idx(-1)
    , move_requires_threshold(false)
    , move_start_threshold_position_2D(Invalid_2D_Point)
{
}

GLCanvas3D::Mouse::Mouse()
    : dragging(false)
    , position(DBL_MAX, DBL_MAX)
    , scene_position(DBL_MAX, DBL_MAX, DBL_MAX)
    , ignore_left_up(false)
    , ignore_right_up(false)
{
}

void GLCanvas3D::Labels::render(const std::vector<const ModelInstance*>& sorted_instances) const
{
    if (!m_enabled || !is_shown() || m_canvas.get_gizmos_manager().is_running())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Model* model = m_canvas.get_model();
    if (model == nullptr)
        return;

    Transform3d world_to_eye = camera.get_view_matrix();
    Transform3d world_to_screen = camera.get_projection_matrix() * world_to_eye;
    const std::array<int, 4>& viewport = camera.get_viewport();

    struct Owner
    {
        int obj_idx;
        int inst_idx;
        size_t model_instance_id;
        BoundingBoxf3 world_box;
        double eye_center_z;
        std::string title;
        std::string label;
        std::string print_order;
        bool selected;
    };

    // collect owners world bounding boxes and data from volumes
    std::vector<Owner> owners;
    const GLVolumeCollection& volumes = m_canvas.get_volumes();
    PartPlate* cur_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    for (const GLVolume* volume : volumes.volumes) {
        int obj_idx = volume->object_idx();
        if (0 <= obj_idx && obj_idx < (int)model->objects.size()) {
            int inst_idx = volume->instance_idx();
            //only show current plate's label
            if (!cur_plate->contain_instance(obj_idx, inst_idx))
                continue;
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [obj_idx, inst_idx](const Owner& owner) {
                return (owner.obj_idx == obj_idx) && (owner.inst_idx == inst_idx);
                });
            if (it != owners.end()) {
                it->world_box.merge(volume->transformed_bounding_box());
                it->selected &= volume->selected;
            } else {
                const ModelObject* model_object = model->objects[obj_idx];
                Owner owner;
                owner.obj_idx = obj_idx;
                owner.inst_idx = inst_idx;
                owner.model_instance_id = model_object->instances[inst_idx]->id().id;
                owner.world_box = volume->transformed_bounding_box();
                owner.title = "object" + std::to_string(obj_idx) + "_inst##" + std::to_string(inst_idx);
                owner.label = model_object->name;
                if (model_object->instances.size() > 1)
                    owner.label += " (" + std::to_string(inst_idx + 1) + ")";
                owner.selected = volume->selected;
                owners.emplace_back(owner);
            }
        }
    }

    // updates print order strings
    if (sorted_instances.size() > 0) {
        for (size_t i = 0; i < sorted_instances.size(); ++i) {
            size_t id = sorted_instances[i]->id().id;
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [id](const Owner& owner) {
                return owner.model_instance_id == id;
                });
            if (it != owners.end())
                //it->print_order = std::string((_(L("Sequence"))).ToUTF8()) + "#: " + std::to_string(i + 1);
                it->print_order = std::string((_(L("Sequence"))).ToUTF8()) + "#: " + std::to_string(sorted_instances[i]->arrange_order);
        }
    }

    // calculate eye bounding boxes center zs
    for (Owner& owner : owners) {
        owner.eye_center_z = (world_to_eye * owner.world_box.center())(2);
    }

    // sort owners by center eye zs and selection
    std::sort(owners.begin(), owners.end(), [](const Owner& owner1, const Owner& owner2) {
        if (!owner1.selected && owner2.selected)
            return true;
        else if (owner1.selected && !owner2.selected)
            return false;
        else
            return (owner1.eye_center_z < owner2.eye_center_z);
        });

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    // render info windows
    for (const Owner& owner : owners) {
        Vec3d screen_box_center = world_to_screen * owner.world_box.center();
        float x = 0.0f;
        float y = 0.0f;
        if (camera.get_type() == Camera::EType::Perspective) {
            x = (0.5f + 0.001f * 0.5f * (float)screen_box_center(0)) * viewport[2];
            y = (0.5f - 0.001f * 0.5f * (float)screen_box_center(1)) * viewport[3];
        } else {
            x = (0.5f + 0.5f * (float)screen_box_center(0)) * viewport[2];
            y = (0.5f - 0.5f * (float)screen_box_center(1)) * viewport[3];
        }

        if (x < 0.0f || viewport[2] < x || y < 0.0f || viewport[3] < y)
            continue;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, owner.selected ? 3.0f : 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, owner.selected ? ImVec4(0.757f, 0.404f, 0.216f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
        imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.5f);
        imgui.begin(owner.title, ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        float win_w = ImGui::GetWindowWidth();
        ImGui::AlignTextToFramePadding();
        imgui.text(owner.label);

        if (!owner.print_order.empty()) {
            ImGui::Separator();
            float po_len = imgui.calc_text_size(owner.print_order).x;
            ImGui::SetCursorPosX(0.5f * (win_w - po_len));
            ImGui::AlignTextToFramePadding();
            imgui.text(owner.print_order);
        }

        // force re-render while the windows gets to its final size (it takes several frames)
        if (ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
            imgui.set_requires_extra_frame();

        imgui.end();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

void GLCanvas3D::Tooltip::set_text(const std::string& text)
{
    // If the mouse is inside an ImGUI dialog, then the tooltip is suppressed.
    m_text = m_in_imgui ? std::string() : text;
}

void GLCanvas3D::Tooltip::render(const Vec2d& mouse_position, GLCanvas3D& canvas)
{
    static ImVec2 size(0.0f, 0.0f);

    auto validate_position = [](const Vec2d& position, const GLCanvas3D& canvas, const ImVec2& wnd_size) {
        const Size cnv_size = canvas.get_canvas_size();
        const float x = std::clamp((float)position.x(), 0.0f, (float)cnv_size.get_width() - wnd_size.x);
        const float y = std::clamp((float)position.y() + 16.0f, 0.0f, (float)cnv_size.get_height() - wnd_size.y);
        return Vec2f(x, y);
    };

    if (m_text.empty()) {
        m_start_time = std::chrono::steady_clock::now();
        return;
    }

    // draw the tooltip as hidden until the delay is expired
    // use a value of alpha slightly different from 0.0f because newer imgui does not calculate properly the window size if alpha == 0.0f
    const float alpha = (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start_time).count() < 500) ? 0.01f : 1.0f;

    const Vec2f position = validate_position(mouse_position, canvas, size);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    imgui.set_next_window_pos(position.x(), position.y(), ImGuiCond_Always, 0.0f, 0.0f);

    imgui.begin(wxString("canvas_tooltip"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextUnformatted(m_text.c_str());

    // force re-render while the windows gets to its final size (it may take several frames) or while hidden
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    if (alpha < 1.0f || ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
        imgui.set_requires_extra_frame();
#else
    if (alpha < 1.0f || ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
        canvas.request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    size = ImGui::GetWindowSize();

    imgui.end();
    ImGui::PopStyleVar(2);
}

//BBS: add height limit logic
void GLCanvas3D::SequentialPrintClearance::set_polygons(const Polygons& polygons, const std::vector<std::pair<Polygon, float>>& height_polygons)
{
    //BBS: add height limit logic
    m_height_limit.reset();
    m_perimeter.reset();
    m_fill.reset();
    if (!polygons.empty()) {
        if (m_render_fill) {
            GLModel::Geometry fill_data;
            fill_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
            fill_data.color  = { 0.8f, 0.8f, 1.0f, 0.5f };

            // vertices + indices
            const ExPolygons polygons_union = union_ex(polygons);
            unsigned int vertices_counter = 0;
            for (const ExPolygon& poly : polygons_union) {
                const std::vector<Vec3d> triangulation = triangulate_expolygon_3d(poly);
                fill_data.reserve_vertices(fill_data.vertices_count() + triangulation.size());
                fill_data.reserve_indices(fill_data.indices_count() + triangulation.size());
                for (const Vec3d& v : triangulation) {
                    fill_data.add_vertex((Vec3f)(v.cast<float>() + 0.0125f * Vec3f::UnitZ())); // add a small positive z to avoid z-fighting
                    ++vertices_counter;
                    if (vertices_counter % 3 == 0)
                        fill_data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
                }
            }

            m_fill.init_from(std::move(fill_data));
        }

        m_perimeter.init_from(polygons, 0.025f); // add a small positive z to avoid z-fighting
    }

    //BBS: add the height limit compute logic
    if (!height_polygons.empty()) {
        GLModel::Geometry height_fill_data;
        height_fill_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        height_fill_data.color  = {0.8f, 0.8f, 1.0f, 0.5f};

        // vertices + indices
        unsigned int vertices_counter = 0;
        for (const auto &poly : height_polygons) {
            ExPolygon                ex_poly(poly.first);
            const std::vector<Vec3d> height_triangulation = triangulate_expolygon_3d(ex_poly);
            for (const Vec3d &v : height_triangulation) {
                Vec3f point{(float) v.x(), (float) v.y(), poly.second};
                height_fill_data.add_vertex(point);
                ++vertices_counter;
                if (vertices_counter % 3 == 0)
                    height_fill_data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
            }
        }

        m_height_limit.init_from(std::move(height_fill_data));
    }
}

void GLCanvas3D::SequentialPrintClearance::render()
{
    const ColorRGBA FILL_COLOR = { 0.7f, 0.7f, 1.0f, 0.5f };
    const ColorRGBA NO_FILL_COLOR = { 0.75f, 0.75f, 0.75f, 0.75f };

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    m_perimeter.set_color(m_render_fill ? FILL_COLOR : NO_FILL_COLOR);
    m_perimeter.render();
    m_fill.render();
    //BBS: add height limit
    m_height_limit.set_color(m_render_fill ? FILL_COLOR : NO_FILL_COLOR);
    m_height_limit.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_DEPTH_TEST));

    shader->stop_using();
}

wxDEFINE_EVENT(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_OBJECT_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_NAME_CHANGE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RIGHT_CLICK, RBtnEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_RIGHT_CLICK, RBtnPlateEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REMOVE_OBJECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);
//BBS: add arrange and orient event
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE_PARTPLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ORIENT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ORIENT_PARTPLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SELECT_CURR_PLATE_ALL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SELECT_ALL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_QUESTION_MARK, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_MOVED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_ROTATED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_SCALED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_FORCE_UPDATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, Event<bool>);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_GEOMETRY, Vec3dsEvent<2>);
wxDEFINE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_BED_SHAPE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_TAB, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESETGIZMOS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOVE_SLIDERS, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_EDIT_COLOR_CHANGE, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_JUMP_TO, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UNDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SWITCH_TO_OBJECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SWITCH_TO_GLOBAL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_COLLAPSE_SIDEBAR, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RELOAD_FROM_DISK, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RENDER_TIMER, wxTimerEvent/*RenderTimerEvent*/);
wxDEFINE_EVENT(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE, SimpleEvent);
wxDEFINE_EVENT(EVT_CUSTOMEVT_TICKSCHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, Event<float>);
wxDEFINE_EVENT(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, HeightProfileSmoothEvent);

const double GLCanvas3D::DefaultCameraZoomToBoxMarginFactor = 1.25;
const double GLCanvas3D::DefaultCameraZoomToBedMarginFactor = 2.00;
const double GLCanvas3D::DefaultCameraZoomToPlateMarginFactor = 1.25;

void GLCanvas3D::load_arrange_settings()
{
    std::string dist_fff_str =
        wxGetApp().app_config->get("arrange", "min_object_distance_fff");

    std::string dist_fff_seq_print_str =
        wxGetApp().app_config->get("arrange", "min_object_distance_seq_print_fff");

    std::string dist_sla_str =
        wxGetApp().app_config->get("arrange", "min_object_distance_sla");

    std::string en_rot_fff_str =
        wxGetApp().app_config->get("arrange", "enable_rotation_fff");

    std::string en_rot_fff_seqp_str =
        wxGetApp().app_config->get("arrange", "enable_rotation_seq_print");

    std::string en_rot_sla_str =
        wxGetApp().app_config->get("arrange", "enable_rotation_sla");

    std::string en_allow_multiple_materials_str =
        wxGetApp().app_config->get("arrange", "allow_multi_materials_on_same_plate");

    std::string en_avoid_region_str =
        wxGetApp().app_config->get("arrange", "avoid_extrusion_cali_region");



    if (!dist_fff_str.empty())
        m_arrange_settings_fff.distance = std::stof(dist_fff_str);

    if (!dist_fff_seq_print_str.empty())
        m_arrange_settings_fff_seq_print.distance = std::stof(dist_fff_seq_print_str);

    if (!dist_sla_str.empty())
        m_arrange_settings_sla.distance = std::stof(dist_sla_str);

    if (!en_rot_fff_str.empty())
        m_arrange_settings_fff.enable_rotation = (en_rot_fff_str == "1" || en_rot_fff_str == "true");

    if (!en_allow_multiple_materials_str.empty())
        m_arrange_settings_fff.allow_multi_materials_on_same_plate = (en_allow_multiple_materials_str == "1" || en_allow_multiple_materials_str == "true");


    if (!en_rot_fff_seqp_str.empty())
        m_arrange_settings_fff_seq_print.enable_rotation = (en_rot_fff_seqp_str == "1" || en_rot_fff_seqp_str == "true");

    if(!en_avoid_region_str.empty())
        m_arrange_settings_fff.avoid_extrusion_cali_region = (en_avoid_region_str == "1" || en_avoid_region_str == "true");

    if (!en_rot_sla_str.empty())
        m_arrange_settings_sla.enable_rotation = (en_rot_sla_str == "1" || en_rot_sla_str == "true");

    //BBS: add specific arrange settings
    m_arrange_settings_fff_seq_print.is_seq_print = true;
}

GLCanvas3D::ArrangeSettings& GLCanvas3D::get_arrange_settings()
{
    PrinterTechnology ptech = current_printer_technology();

    auto* ptr = &m_arrange_settings_fff;

    if (ptech == ptSLA) {
        ptr = &m_arrange_settings_sla;
    }
    else if (ptech == ptFFF) {
        if (wxGetApp().global_print_sequence() == PrintSequence::ByObject)
            ptr = &m_arrange_settings_fff_seq_print;
        else
            ptr = &m_arrange_settings_fff;
    }

    return *ptr;
}

int GLCanvas3D::GetHoverId()
{
    if (m_hover_plate_idxs.size() == 0) {
        return -1; }
    return m_hover_plate_idxs.front();

}

PrinterTechnology GLCanvas3D::current_printer_technology() const {
    return m_process->current_printer_technology();
}

GLCanvas3D::GLCanvas3D(wxGLCanvas* canvas, Bed3D &bed)
    : m_canvas(canvas)
    , m_context(nullptr)
    , m_bed(bed)
#if ENABLE_RETINA_GL
    , m_retina_helper(nullptr)
#endif
    , m_in_render(false)
    , m_main_toolbar(GLToolbar::Normal, "Main")
    , m_separator_toolbar(GLToolbar::Normal, "Separator")
    , m_assemble_view_toolbar(GLToolbar::Normal, "Assembly_View")
    , m_return_toolbar()
    , m_canvas_type(ECanvasType::CanvasView3D)
    , m_gizmos(*this)
    , m_use_clipping_planes(false)
    , m_sidebar_field("")
    , m_extra_frame_requested(false)
    , m_config(nullptr)
    , m_process(nullptr)
    , m_model(nullptr)
    , m_dirty(true)
    , m_initialized(false)
    , m_apply_zoom_to_volumes_filter(false)
    , m_picking_enabled(false)
    , m_moving_enabled(false)
    , m_dynamic_background_enabled(false)
    , m_multisample_allowed(false)
    , m_moving(false)
    , m_tab_down(false)
    , m_camera_movement(false)
    , m_cursor_type(Standard)
    , m_color_by("volume")
    , m_reload_delayed(false)
    , m_render_sla_auxiliaries(true)
    , m_labels(*this)
    , m_slope(m_volumes)
{
    if (m_canvas != nullptr) {
        m_timer.SetOwner(m_canvas);
        m_render_timer.SetOwner(m_canvas);
#if ENABLE_RETINA_GL
        m_retina_helper.reset(new RetinaHelper(canvas));
#endif // ENABLE_RETINA_GL
    }
    m_timer_set_color.Bind(wxEVT_TIMER, &GLCanvas3D::on_set_color_timer, this);
    load_arrange_settings();

    m_selection.set_volumes(&m_volumes.volumes);

    m_assembly_view_desc["object_selection_caption"] = _L("Left mouse button");
    m_assembly_view_desc["object_selection"]         = _L("object selection");
    // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
    m_assembly_view_desc["part_selection_caption"]   = _L("Alt+") + _L("Left mouse button");
    m_assembly_view_desc["part_selection"]           = _L("part selection");
    m_assembly_view_desc["number_key_caption"]       = "1~16 " + _L("number keys");
    m_assembly_view_desc["number_key"]       = _L("number keys can quickly change the color of objects");
}

GLCanvas3D::~GLCanvas3D()
{
    reset_volumes();

    m_sel_plate_toolbar.del_all_item();
    m_sel_plate_toolbar.del_stats_item();
}

void GLCanvas3D::post_event(wxEvent &&event)
{
    event.SetEventObject(m_canvas);
    wxPostEvent(m_canvas, event);
}

bool GLCanvas3D::init()
{
    if (m_initialized)
        return true;

    if (m_canvas == nullptr || m_context == nullptr)
        return false;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< " enter";
    glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    glsafe(::glClearDepth(1.0f));

    glsafe(::glDepthFunc(GL_LESS));

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    if (m_multisample_allowed)
        glsafe(::glEnable(GL_MULTISAMPLE));

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": before m_layers_editing init";
    if (m_main_toolbar.is_enabled())
        m_layers_editing.init();

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": before gizmo init";
    if (m_gizmos.is_enabled() && !m_gizmos.init())
        std::cout << "Unable to initialize gizmos: please, check that all the required textures are available" << std::endl;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": before _init_toolbars";
    if (!_init_toolbars())
        return false;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": finish _init_toolbars";
    if (m_selection.is_enabled() && !m_selection.init())
        return false;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": finish m_selection";

#if ENABLE_IMGUI_STYLE_EDITOR
    //BBS load render color for style editor
    GLVolume::load_render_colors();
    PartPlate::load_render_colors();
    GLGizmoBase::load_render_colors();
    GLCanvas3D::load_render_colors();
    Bed3D::load_render_colors();
#endif
    //if (!wxGetApp().is_gl_version_greater_or_equal_to(3, 0))
    //    wxGetApp().plater()->enable_wireframe(false);
    m_initialized = true;

    return true;
}

void GLCanvas3D::set_as_dirty()
{
    m_dirty = true;
}

const float GLCanvas3D::get_scale() const
{
#if ENABLE_RETINA_GL
    return m_retina_helper->get_scale_factor();
#else
    return 1.0f;
#endif
}

unsigned int GLCanvas3D::get_volumes_count() const
{
    return (unsigned int)m_volumes.volumes.size();
}

void GLCanvas3D::reset_volumes()
{
    if (!m_initialized)
        return;

    if (m_volumes.empty())
        return;

    _set_current();

    m_selection.clear();
    m_volumes.clear();
    m_dirty = true;

    _set_warning_notification(EWarning::ObjectOutside, false);
}

//BBS: get current plater's bounding box
BoundingBoxf3 GLCanvas3D::_get_current_partplate_print_volume()
{
    BoundingBoxf3 test_volume;
    if (m_process && m_config)
    {
        BoundingBoxf3 plate_bb = m_process->get_current_plate()->get_bounding_box(false);
        BoundingBoxf3 print_volume({ plate_bb.min(0), plate_bb.min(1), 0.0 }, { plate_bb.max(0), plate_bb.max(1), m_config->opt_float("printable_height") });
        // Allow the objects to protrude below the print bed
        print_volume.min(2) = -1e10;
        print_volume.min(0) -= Slic3r::BuildVolume::BedEpsilon;
        print_volume.min(1) -= Slic3r::BuildVolume::BedEpsilon;
        print_volume.max(0) += Slic3r::BuildVolume::BedEpsilon;
        print_volume.max(1) += Slic3r::BuildVolume::BedEpsilon;
        test_volume = print_volume;
    }
    else
        test_volume = BoundingBoxf3();

    return test_volume;
}

ModelInstanceEPrintVolumeState GLCanvas3D::check_volumes_outside_state() const
{
    //BBS: if not initialized, return inside directly insteadof assert
    if (!m_initialized) {
        return ModelInstancePVS_Inside;
    }
    //assert(m_initialized);

    ModelInstanceEPrintVolumeState state;
    m_volumes.check_outside_state(m_bed.build_volume(), &state);
    return state;
}

void GLCanvas3D::toggle_selected_volume_visibility(bool selected_visible)
{
    m_render_sla_auxiliaries = !selected_visible;
    if (selected_visible) {
        const Selection::IndicesList &idxs = m_selection.get_volume_idxs();
        if (idxs.size() > 0) {
            for (GLVolume *vol : m_volumes.volumes) {
                if (vol->composite_id.object_id >= 1000 && vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
                    continue; // the wipe tower
                if (vol->composite_id.volume_id >= 0) {
                    vol->is_active = false;
                }
            }
            for (unsigned int idx : idxs) {
                GLVolume *v  = const_cast<GLVolume *>(m_selection.get_volume(idx));
                v->is_active = true;
            }
        }
    } else { // show all
        for (GLVolume *vol : m_volumes.volumes) {
            if (vol->composite_id.object_id >= 1000 && vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
                continue; // the wipe tower
            if (vol->composite_id.volume_id >= 0) {
                vol->is_active = true;
            }
        }
    }
}

void GLCanvas3D::toggle_sla_auxiliaries_visibility(bool visible, const ModelObject *mo, int instance_idx)
{
    if (current_printer_technology() != ptSLA)
        return;

    m_render_sla_auxiliaries = visible;

    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = get_raycasters_for_picking(SceneRaycaster::EType::Volume);

    for (GLVolume* vol : m_volumes.volumes) {
        if (vol->composite_id.object_id >= 1000 &&
            vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
            continue; // the wipe tower
      if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
            && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)
            && vol->composite_id.volume_id < 0) {
            vol->is_active = visible;
            auto it = std::find_if(raycasters->begin(), raycasters->end(), [vol](std::shared_ptr<SceneRaycasterItem> item) { return item->get_raycaster() == vol->mesh_raycaster.get(); });
            if (it != raycasters->end())
                (*it)->set_active(vol->is_active);
        }
    }
}

void GLCanvas3D::toggle_model_objects_visibility(bool visible, const ModelObject* mo, int instance_idx, const ModelVolume* mv)
{
    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = get_raycasters_for_picking(SceneRaycaster::EType::Volume);
    for (GLVolume* vol : m_volumes.volumes) {
        // BBS: add partplate logic
        if (vol->composite_id.object_id >= 1000 &&
            vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count()) { // wipe tower
            vol->is_active = (visible && mo == nullptr);
        }
        else {
            if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
            && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)
            && (mv == nullptr || m_model->objects[vol->composite_id.object_id]->volumes[vol->composite_id.volume_id] == mv)) {
                vol->is_active = visible;
                if (!vol->is_modifier)
                    vol->color.a(1.f);

                if (instance_idx == -1) {
                    vol->force_native_color = false;
                    vol->force_neutral_color = false;
                } else {
                    const GLGizmosManager& gm = get_gizmos_manager();
                    auto gizmo_type = gm.get_current_type();
                    if (  (gizmo_type == GLGizmosManager::FdmSupports
                        || gizmo_type == GLGizmosManager::Seam
                        || gizmo_type == GLGizmosManager::Cut
                        || gizmo_type == GLGizmosManager::FuzzySkin)
                        && !vol->is_modifier) {
                        vol->force_neutral_color = true;
                    }
                    else if (gizmo_type == GLGizmosManager::BrimEars)
                        vol->force_neutral_color = false;
                    else if (gizmo_type == GLGizmosManager::MmSegmentation)
                        vol->is_active = false;
                    else
                        vol->force_native_color = true;
                }
            }
        }

        auto it = std::find_if(raycasters->begin(), raycasters->end(), [vol](std::shared_ptr<SceneRaycasterItem> item) { return item->get_raycaster() == vol->mesh_raycaster.get(); });
        if (it != raycasters->end())
            (*it)->set_active(vol->is_active);
    }

    if (visible && !mo)
        toggle_sla_auxiliaries_visibility(true, mo, instance_idx);

    if (!mo && !visible && !m_model->objects.empty() && (m_model->objects.size() > 1 || m_model->objects.front()->instances.size() > 1))
        _set_warning_notification(EWarning::SomethingNotShown, true);

    if (!mo && visible)
        _set_warning_notification(EWarning::SomethingNotShown, false);
}

void GLCanvas3D::update_instance_printable_state_for_object(const size_t obj_idx)
{
    ModelObject* model_object = m_model->objects[obj_idx];
    for (int inst_idx = 0; inst_idx < (int)model_object->instances.size(); ++inst_idx) {
        ModelInstance* instance = model_object->instances[inst_idx];

        for (GLVolume* volume : m_volumes.volumes) {
            if ((volume->object_idx() == (int)obj_idx) && (volume->instance_idx() == inst_idx))
                volume->printable = instance->printable;
                if (!volume->printable) {
                    volume->render_color = GLVolume::UNPRINTABLE_COLOR;
                }
        }
    }
}

void GLCanvas3D::update_instance_printable_state_for_objects(const std::vector<size_t>& object_idxs)
{
    for (size_t obj_idx : object_idxs)
        update_instance_printable_state_for_object(obj_idx);
}

void GLCanvas3D::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    m_layers_editing.set_config(config);

    // Orca: Filament shrinkage compensation
    const Print *print = fff_print();
    if (print != nullptr)
        m_layers_editing.set_shrinkage_compensation(fff_print()->shrinkage_compensation());
}

void GLCanvas3D::set_process(BackgroundSlicingProcess *process)
{
    m_process = process;
}

void GLCanvas3D::set_model(Model* model)
{
    m_model = model;
    m_selection.set_model(m_model);
}

void GLCanvas3D::bed_shape_changed()
{
    refresh_camera_scene_box();
    wxGetApp().plater()->get_camera().requires_zoom_to_bed = true;
    m_dirty = true;
}

void GLCanvas3D::plates_count_changed()
{
    refresh_camera_scene_box();
    m_dirty = true;
}

Camera& GLCanvas3D::get_camera()
{
    return camera;
}

void GLCanvas3D::set_color_by(const std::string& value)
{
    m_color_by = value;
}

void GLCanvas3D::refresh_camera_scene_box()
{
    wxGetApp().plater()->get_camera().set_scene_box(scene_bounding_box());
}

BoundingBoxf3 GLCanvas3D::volumes_bounding_box(bool current_plate_only) const
{
    BoundingBoxf3 bb;
    BoundingBoxf3 expand_part_plate_list_box;
    bool          is_limit = m_canvas_type != ECanvasType::CanvasAssembleView;
    if (is_limit) {
        if (current_plate_only) {
            expand_part_plate_list_box = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_bounding_box();
        } else {
            auto        plate_list_box = wxGetApp().plater()->get_partplate_list().get_bounding_box();
            auto        horizontal_radius = 0.5 * sqrt(std::pow(plate_list_box.min[0] - plate_list_box.max[0], 2) + std::pow(plate_list_box.min[1] - plate_list_box.max[1], 2));
            const float scale             = 2;
            expand_part_plate_list_box.merge(plate_list_box.min - scale * Vec3d(horizontal_radius, horizontal_radius, 0));
            expand_part_plate_list_box.merge(plate_list_box.max + scale * Vec3d(horizontal_radius, horizontal_radius, 0));
        }
    }
    for (const GLVolume *volume : m_volumes.volumes) {
        if (!m_apply_zoom_to_volumes_filter || ((volume != nullptr) && volume->zoom_to_volumes)) {
            const auto v_bb     = volume->transformed_bounding_box();
            if (is_limit && !expand_part_plate_list_box.overlap(v_bb))
                continue;
            bb.merge(v_bb);
        }
    }
    return bb;
}

BoundingBoxf3 GLCanvas3D::scene_bounding_box() const
{
    BoundingBoxf3 bb = volumes_bounding_box();
    bb.merge(m_bed.extended_bounding_box());
    double h = m_bed.build_volume().printable_height();
    //FIXME why -h?
    bb.min.z() = std::min(bb.min.z(), -h);
    bb.max.z() = std::max(bb.max.z(), h);

    //BBS merge plate scene bounding box
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        PartPlateList& plate = wxGetApp().plater()->get_partplate_list();
        bb.merge(plate.get_bounding_box());
    }

    return bb;
}

BoundingBoxf3 GLCanvas3D::plate_scene_bounding_box(int plate_idx) const
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);

    BoundingBoxf3 bb = plate->get_bounding_box(true);
    if (m_config != nullptr) {
        double h = m_config->opt_float("printable_height");
        bb.min(2) = std::min(bb.min(2), -h);
        bb.max(2) = std::max(bb.max(2), h);
    }

    return bb;
}

bool GLCanvas3D::is_layers_editing_enabled() const
{
    return m_layers_editing.is_enabled();
}

bool GLCanvas3D::is_layers_editing_allowed() const
{
    return m_layers_editing.is_allowed();
}

void GLCanvas3D::reset_layer_height_profile()
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Reset");
    m_layers_editing.reset_layer_height_profile(*this);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::adaptive_layer_height_profile(float quality_factor)
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Adaptive");
    m_layers_editing.adaptive_layer_height_profile(*this, quality_factor);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::smooth_layer_height_profile(const HeightProfileSmoothingParams& smoothing_params)
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Smooth all");
    m_layers_editing.smooth_layer_height_profile(*this, smoothing_params);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

bool GLCanvas3D::is_reload_delayed() const
{
    return m_reload_delayed;
}

void GLCanvas3D::enable_layers_editing(bool enable)
{
    m_layers_editing.set_enabled(enable);
    set_as_dirty();
}

void GLCanvas3D::enable_legend_texture(bool enable)
{
    m_gcode_viewer.enable_legend(enable);
}

void GLCanvas3D::enable_picking(bool enable)
{
    m_picking_enabled = enable;

    // Orca: invalidate hover state when dragging is toggled, otherwise if we turned off dragging
    // while hovering above a volume, the hovering state won't update even if mouse has moved away.
    // Fixes https://github.com/SoftFever/OrcaSlicer/pull/9979#issuecomment-3065575889
    m_hover_volume_idxs.clear();
}

void GLCanvas3D::enable_moving(bool enable)
{
    m_moving_enabled = enable;
}

void GLCanvas3D::enable_gizmos(bool enable)
{
    m_gizmos.set_enabled(enable);
}

void GLCanvas3D::enable_selection(bool enable)
{
    m_selection.set_enabled(enable);
}

void GLCanvas3D::enable_main_toolbar(bool enable)
{
    m_main_toolbar.set_enabled(enable);
}

void GLCanvas3D::reset_select_plate_toolbar_selection() {
    if (m_sel_plate_toolbar.m_all_plates_stats_item)
        m_sel_plate_toolbar.m_all_plates_stats_item->selected = false;
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->update_slice_print_status(MainFrame::eEventSliceUpdate, true, true);
}

void GLCanvas3D::enable_select_plate_toolbar(bool enable)
{
    m_sel_plate_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_assemble_view_toolbar(bool enable)
{
    m_assemble_view_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_return_toolbar(bool enable)
{
    m_return_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_separator_toolbar(bool enable)
{
    m_separator_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_dynamic_background(bool enable)
{
    m_dynamic_background_enabled = enable;
}

void GLCanvas3D::allow_multisample(bool allow)
{
    m_multisample_allowed = allow;
}

void GLCanvas3D::zoom_to_bed()
{
    BoundingBoxf3 box = m_bed.build_volume().bounding_volume();
    box.min.z() = 0.0;
    box.max.z() = 0.0;
    _zoom_to_box(box, DefaultCameraZoomToBedMarginFactor);
}

void GLCanvas3D::zoom_to_volumes()
{
    m_apply_zoom_to_volumes_filter = true;
    _zoom_to_box(volumes_bounding_box());
    m_apply_zoom_to_volumes_filter = false;
}

void GLCanvas3D::zoom_to_selection()
{
    if (!m_selection.is_empty())
        _zoom_to_box(m_selection.get_bounding_box());
}

void GLCanvas3D::zoom_to_gcode()
{
    _zoom_to_box(m_gcode_viewer.get_paths_bounding_box(), 1.05);
}

void GLCanvas3D::zoom_to_plate(int plate_idx)
{
    BoundingBoxf3 box;
    if (plate_idx == REQUIRES_ZOOM_TO_ALL_PLATE) {
        box = wxGetApp().plater()->get_partplate_list().get_bounding_box();
        box.min.z() = 0.0;
        box.max.z() = 0.0;
        _zoom_to_box(box, DefaultCameraZoomToPlateMarginFactor);
    } else {
        PartPlate* plate = nullptr;
        if (plate_idx == REQUIRES_ZOOM_TO_CUR_PLATE) {
            plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        }else {
            assert(plate_idx >= 0 && plate_idx < wxGetApp().plater()->get_partplate_list().get_plate_count());
            plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
        }
        box = plate->get_bounding_box(true);
        box.min.z() = 0.0;
        box.max.z() = 0.0;
        _zoom_to_box(box, DefaultCameraZoomToPlateMarginFactor);
    }
}

void GLCanvas3D::select_view(const std::string& direction)
{
    wxGetApp().plater()->get_camera().select_view(direction);
    if (m_canvas != nullptr)
        m_canvas->Refresh();
}

void GLCanvas3D::select_plate()
{
    wxGetApp().plater()->get_partplate_list().select_plate_view();
    if (m_canvas != nullptr)
        m_canvas->Refresh();
}

void GLCanvas3D::update_volumes_colors_by_extruder()
{
    if (m_config != nullptr)
        m_volumes.update_colors_by_extruder(m_config);
}

bool GLCanvas3D::is_collapse_toolbar_on_left() const
{
    auto state = wxGetApp().plater()->get_sidebar_docking_state();
    return state == Sidebar::Left;
}

float GLCanvas3D::get_collapse_toolbar_width() const
{
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    const auto state            = wxGetApp().plater()->get_sidebar_docking_state();

    return state != Sidebar::None ? collapse_toolbar.get_width() : 0;
}

float GLCanvas3D::get_collapse_toolbar_height() const
{
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    const auto state            = wxGetApp().plater()->get_sidebar_docking_state();

    return state != Sidebar::None ? collapse_toolbar.get_height() : 0;
}

bool GLCanvas3D::make_current_for_postinit() {
    return _set_current();
}

void GLCanvas3D::render(bool only_init)
{
    if (m_in_render) {
        // if called recursively, return
        m_dirty = true;
        return;
    }

    m_in_render = true;
    Slic3r::ScopeGuard in_render_guard([this]() { m_in_render = false; });
    (void)in_render_guard;

    if (m_canvas == nullptr)
        return;

    //BBS: add enable_render
    if (!m_enable_render)
        return;

    // ensures this canvas is current and initialized
    if (!_is_shown_on_screen() || !_set_current() || !wxGetApp().init_opengl())
        return;

    if (!is_initialized() && !init())
        return;
    if (m_canvas_type == ECanvasType::CanvasView3D  && m_gizmos.get_current_type() == GLGizmosManager::Undefined) {
        enable_return_toolbar(false);
    }
    if (!m_main_toolbar.is_enabled())
        m_gcode_viewer.init(wxGetApp().get_mode(), wxGetApp().preset_bundle);

    if (! m_bed.build_volume().valid()) {
        // this happens at startup when no data is still saved under <>\AppData\Roaming\Slic3rPE
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE_BED_SHAPE));
        return;
    }

    if (only_init)
        return;

#if ENABLE_ENVIRONMENT_MAP
    if (wxGetApp().is_editor())
        wxGetApp().plater()->init_environment_texture();
#endif // ENABLE_ENVIRONMENT_MAP

    const Size& cnv_size = get_canvas_size();
    // Probably due to different order of events on Linux/GTK2, when one switched from 3D scene
    // to preview, this was called before canvas had its final size. It reported zero width
    // and the viewport was set incorrectly, leading to tripping glAsserts further down
    // the road (in apply_projection). That's why the minimum size is forced to 10.
    Camera& camera = wxGetApp().plater()->get_camera();
    camera.set_viewport(0, 0, std::max(10u, (unsigned int)cnv_size.get_width()), std::max(10u, (unsigned int)cnv_size.get_height()));
    camera.apply_viewport();

    if (camera.requires_zoom_to_bed) {
        zoom_to_bed();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_bed = false;
    }

    if (camera.requires_zoom_to_plate > REQUIRES_ZOOM_TO_PLATE_IDLE) {
        zoom_to_plate(camera.requires_zoom_to_plate);
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_plate = REQUIRES_ZOOM_TO_PLATE_IDLE;
    }

    if (camera.requires_zoom_to_volumes) {
        zoom_to_volumes();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_volumes = false;
    }

    camera.apply_projection(_max_bounding_box(true, true, true));

    wxGetApp().imgui()->new_frame();

    if (m_picking_enabled) {
        if (m_rectangle_selection.is_dragging())
            // picking pass using rectangle selection
            _rectangular_selection_picking_pass();
        //BBS: enable picking when no volumes for partplate logic
        //else if (!m_volumes.empty())
        else {
            // regular picking pass
            _picking_pass();

#if ENABLE_RAYCAST_PICKING_DEBUG
            ImGuiWrapper& imgui = *wxGetApp().imgui();
            imgui.begin(std::string("Hit result"), ImGuiWindowFlags_AlwaysAutoResize);
            imgui.text("Picking disabled");
            imgui.end();
#endif // ENABLE_RAYCAST_PICKING_DEBUG
        }
    }

    // draw scene
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    _render_background();

    //BBS add partplater rendering logic
    bool only_current = false, only_body = false, show_axes = true, no_partplate = false;
    bool show_grid = true;
    GLGizmosManager::EType gizmo_type = m_gizmos.get_current_type();
    if (!m_main_toolbar.is_enabled()) {
        //only_body = true;
        only_current = true;
    }
    else if ((gizmo_type == GLGizmosManager::FdmSupports) || (gizmo_type == GLGizmosManager::Seam) || (gizmo_type == GLGizmosManager::MmSegmentation) || (gizmo_type == GLGizmosManager::FuzzySkin))
        no_partplate = true;
    else if (gizmo_type == GLGizmosManager::BrimEars && !camera.is_looking_downward())
        show_grid = false;

    /* view3D render*/
    int hover_id = (m_hover_plate_idxs.size() > 0)?m_hover_plate_idxs.front():-1;
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        //BBS: add outline logic
        _render_objects(GLVolumeCollection::ERenderType::Opaque, !m_gizmos.is_running());
        _render_sla_slices();
        _render_selection();
        if (!no_partplate)
            _render_bed(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), show_axes);
        if (!no_partplate) //BBS: add outline logic
            _render_platelist(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), only_current, only_body, hover_id, true, show_grid);
        _render_objects(GLVolumeCollection::ERenderType::Transparent, !m_gizmos.is_running());
    }
    /* preview render */
    else if (m_canvas_type == ECanvasType::CanvasPreview && m_render_preview) {
        _render_objects(GLVolumeCollection::ERenderType::Opaque, !m_gizmos.is_running());
        _render_sla_slices();
        _render_selection();
        _render_bed(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), show_axes);
        _render_platelist(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), only_current, true, hover_id);
        // BBS: GUI refactor: add canvas size as parameters
        _render_gcode(cnv_size.get_width(), cnv_size.get_height());
    }
    /* assemble render*/
    else if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        //BBS: add outline logic
        if (m_show_world_axes) {
            m_axes.render();
        }
        _render_objects(GLVolumeCollection::ERenderType::Opaque, !m_gizmos.is_running());
        _render_selection();
        //_render_bed(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), show_axes);
        _render_plane();
        //BBS: add outline logic insteadof selection under assemble view
        //_render_selection();
        // BBS: add outline logic
        _render_objects(GLVolumeCollection::ERenderType::Transparent, !m_gizmos.is_running());
    }

    _render_sequential_clearance();
#if ENABLE_RENDER_SELECTION_CENTER
    _render_selection_center();
#endif // ENABLE_RENDER_SELECTION_CENTER

    // we need to set the mouse's scene position here because the depth buffer
    // could be invalidated by the following gizmo render methods
    // this position is used later into on_mouse() to drag the objects
    if (m_picking_enabled)
        m_mouse.scene_position = _mouse_to_3d(m_mouse.position.cast<coord_t>());

    // sidebar hints need to be rendered before the gizmos because the depth buffer
    // could be invalidated by the following gizmo render methods
    _render_selection_sidebar_hints();
    _render_current_gizmo();

#if ENABLE_RAYCAST_PICKING_DEBUG
    if (m_picking_enabled && !m_mouse.dragging && !m_gizmos.is_dragging() && !m_rectangle_selection.is_dragging())
        m_scene_raycaster.render_hit(camera);
#endif // ENABLE_RAYCAST_PICKING_DEBUG

#if ENABLE_SHOW_CAMERA_TARGET
    _render_camera_target();
#endif // ENABLE_SHOW_CAMERA_TARGET

    if (m_picking_enabled && m_rectangle_selection.is_dragging())
        m_rectangle_selection.render(*this);

    // draw overlays
    _render_overlays();

    if (wxGetApp().plater()->is_render_statistic_dialog_visible()) {
        ImGui::ShowMetricsWindow();

        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.begin(std::string("Render statistics"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        imgui.text("FPS (SwapBuffers() calls per second):");
        ImGui::SameLine();
        imgui.text(std::to_string(m_render_stats.get_fps_and_reset_if_needed()));
        ImGui::Separator();
        imgui.text("Compressed textures:");
        ImGui::SameLine();
        imgui.text(OpenGLManager::are_compressed_textures_supported() ? "supported" : "not supported");
        imgui.text("Max texture size:");
        ImGui::SameLine();
        imgui.text(std::to_string(OpenGLManager::get_gl_info().get_max_tex_size()));
        imgui.end();
    }

#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
    if (wxGetApp().is_editor() && wxGetApp().plater()->is_view3D_shown())
        wxGetApp().plater()->render_project_state_debug_window();
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

#if ENABLE_CAMERA_STATISTICS
    camera.debug_render();
#endif // ENABLE_CAMERA_STATISTICS

#if ENABLE_IMGUI_STYLE_EDITOR
    if (wxGetApp().get_mode() == ConfigOptionMode::comDevelop)
        _render_style_editor();
#endif


    std::string tooltip;

	// Negative coordinate means out of the window, likely because the window was deactivated.
	// In that case the tooltip should be hidden.
    if (m_mouse.position.x() >= 0. && m_mouse.position.y() >= 0.) {
        if (tooltip.empty())
            tooltip = m_layers_editing.get_tooltip(*this);

	    if (tooltip.empty())
	        tooltip = m_gizmos.get_tooltip();

	    if (tooltip.empty())
	        tooltip = m_main_toolbar.get_tooltip();

        //BBS: GUI refactor: GLToolbar
        if (tooltip.empty())
            tooltip = m_assemble_view_toolbar.get_tooltip();

	    if (tooltip.empty())
            tooltip = wxGetApp().plater()->get_collapse_toolbar().get_tooltip();

        // BBS
#if 0
	    if (tooltip.empty())
            tooltip = wxGetApp().plater()->get_view_toolbar().get_tooltip();
#endif
    }

    set_tooltip(tooltip);

    if (m_tooltip_enabled)
        m_tooltip.render(m_mouse.position, *this);

    wxGetApp().plater()->get_mouse3d_controller().render_settings_dialog(*this);

    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        float right_margin = SLIDER_DEFAULT_RIGHT_MARGIN;
        float bottom_margin = SLIDER_DEFAULT_BOTTOM_MARGIN;
        if (m_canvas_type == ECanvasType::CanvasPreview) {
            float scale_factor = get_scale();
#ifdef WIN32
            int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
            scale_factor *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32
            right_margin = SLIDER_RIGHT_MARGIN * scale_factor * GCODE_VIEWER_SLIDER_SCALE;
            bottom_margin = SLIDER_BOTTOM_MARGIN * scale_factor * GCODE_VIEWER_SLIDER_SCALE;
        }
        wxGetApp().plater()->get_notification_manager()->render_notifications(*this, get_overlay_window_width(), bottom_margin, right_margin);
        wxGetApp().plater()->get_dailytips()->render();
    }

    wxGetApp().imgui()->render();

    m_canvas->SwapBuffers();
    m_render_stats.increment_fps_counter();
}

void GLCanvas3D::render_thumbnail(ThumbnailData &         thumbnail_data,
                                  unsigned int            w,
                                  unsigned int            h,
                                  const ThumbnailsParams &thumbnail_params,
                                  Camera::EType           camera_type,
                                  bool                    use_top_view,
                                  bool                    for_picking,
                                  bool                    ban_light)
{
    render_thumbnail(thumbnail_data, w, h, thumbnail_params, m_volumes, camera_type, use_top_view, for_picking, ban_light);
}

void GLCanvas3D::render_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
                                  const GLVolumeCollection &volumes,
                                  Camera::EType             camera_type,
                                  bool                      use_top_view,
                                  bool                      for_picking,
                                  bool                      ban_light)
{
    GLShaderProgram* shader = nullptr;
    if (for_picking)
        shader = wxGetApp().get_shader("flat");
    else
        shader = wxGetApp().get_shader("thumbnail");
    ModelObjectPtrs& model_objects = GUI::wxGetApp().model().objects;
    std::vector<ColorRGBA> colors = ::get_extruders_colors();
    switch (OpenGLManager::get_framebuffers_type())
    {
    case OpenGLManager::EFramebufferType::Arb:
        { render_thumbnail_framebuffer(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, colors, shader, camera_type,
                                     use_top_view, for_picking, ban_light);
        break;
    }
    case OpenGLManager::EFramebufferType::Ext:
        { render_thumbnail_framebuffer_ext(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, colors, shader, camera_type,
                                         use_top_view, for_picking, ban_light);
        break;
    }
    default:
        { render_thumbnail_legacy(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, colors, shader, camera_type);
        break;
    }
    }
}

void GLCanvas3D::render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params)
{
    //load current plate gcode
    m_gcode_viewer.render_calibration_thumbnail(thumbnail_data, w, h, thumbnail_params,
        wxGetApp().plater()->get_partplate_list(), wxGetApp().get_opengl_manager());
}

//BBS
void GLCanvas3D::select_curr_plate_all()
{
    m_selection.add_curr_plate();
    m_dirty = true;
}

void GLCanvas3D::select_object_from_idx(std::vector<int>& object_idxs) {
    m_selection.add_object_from_idx(object_idxs);
    m_dirty = true;
}

//BBS
void GLCanvas3D::remove_curr_plate_all()
{
    m_selection.remove_curr_plate();
    m_dirty = true;
}

void GLCanvas3D::update_plate_thumbnails()
{
    _update_imgui_select_plate_toolbar();
}

void GLCanvas3D::select_all()
{
    if (!m_gizmos.is_allow_select_all()) {
        return;
    }
    m_selection.add_all();
    m_dirty = true;
}

void GLCanvas3D::deselect_all()
{
    m_selection.remove_all();
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();
    m_gizmos.reset_all_states();
    m_gizmos.update_data();
    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
}

void GLCanvas3D::exit_gizmo() {
    if (m_gizmos.get_current_type() != GLGizmosManager::Undefined) {
        m_gizmos.reset_all_states();
        m_gizmos.update_data();
    }
}

void GLCanvas3D::set_selected_visible(bool visible)
{
    for (unsigned int i : m_selection.get_volume_idxs()) {
        GLVolume* volume = const_cast<GLVolume*>(m_selection.get_volume(i));
        volume->visible = visible;
        volume->color.a(visible ? 1.f : GLVolume::MODEL_HIDDEN_COL.a());
        volume->render_color.a(volume->color.a());
        volume->force_native_color = !visible;
    }
}

void GLCanvas3D::delete_selected()
{
    m_selection.erase();
}

void GLCanvas3D::ensure_on_bed(unsigned int object_idx, bool allow_negative_z)
{
    //BBS if asseble view canvas
    if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        return;
    }

    if (allow_negative_z)
        return;

    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_min_z;

    for (GLVolume* volume : m_volumes.volumes) {
        if (volume->object_idx() == (int)object_idx && !volume->is_modifier) {
            double min_z = volume->transformed_convex_hull_bounding_box().min.z();
            std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_min_z.find(instance);
            if (it == instances_min_z.end())
                it = instances_min_z.insert(InstancesToZMap::value_type(instance, DBL_MAX)).first;

            it->second = std::min(it->second, min_z);
        }
    }

    for (GLVolume* volume : m_volumes.volumes) {
        std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
        InstancesToZMap::iterator it = instances_min_z.find(instance);
        if (it != instances_min_z.end())
            volume->set_instance_offset(Z, volume->get_instance_offset(Z) - it->second);
    }
}


const std::vector<double>& GLCanvas3D::get_gcode_layers_zs() const
{
    return m_gcode_viewer.get_layers_zs();
}

std::vector<double> GLCanvas3D::get_volumes_print_zs(bool active_only) const
{
    return m_volumes.get_current_print_zs(active_only);
}

void GLCanvas3D::set_gcode_options_visibility_from_flags(unsigned int flags)
{
    m_gcode_viewer.set_options_visibility_from_flags(flags);
}

void GLCanvas3D::set_volumes_z_range(const std::array<double, 2>& range)
{
    m_volumes.set_range(range[0] - 1e-6, range[1] + 1e-6);
}

std::vector<int> GLCanvas3D::load_object(const ModelObject& model_object, int obj_idx, std::vector<int> instance_idxs)
{
    if (instance_idxs.empty()) {
        for (unsigned int i = 0; i < model_object.instances.size(); ++i) {
            instance_idxs.emplace_back(i);
        }
    }
    return m_volumes.load_object(&model_object, obj_idx, instance_idxs, m_color_by, m_initialized);
}

std::vector<int> GLCanvas3D::load_object(const Model& model, int obj_idx)
{
    if (0 <= obj_idx && obj_idx < (int)model.objects.size()) {
        const ModelObject* model_object = model.objects[obj_idx];
        if (model_object != nullptr)
            return load_object(*model_object, obj_idx, std::vector<int>());
    }

    return std::vector<int>();
}

void GLCanvas3D::mirror_selection(Axis axis)
{
    TransformationType transformation_type;
    //transformation_type.set_world();
    transformation_type.set_relative();
    m_selection.setup_cache();
    m_selection.mirror(axis, transformation_type);
    do_mirror(L("Mirror Object"));
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();
}

// Reload the 3D scene of
// 1) Model / ModelObjects / ModelInstances / ModelVolumes
// 2) Print bed
// 3) SLA support meshes for their respective ModelObjects / ModelInstances
// 4) Wipe tower preview
// 5) Out of bed collision status & message overlay (texture)
void GLCanvas3D::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas == nullptr || m_config == nullptr || m_model == nullptr)
        return;

    if (!m_initialized)
        return;

    _set_current();

    m_hover_volume_idxs.clear();

    struct ModelVolumeState {
        ModelVolumeState(const GLVolume* volume) :
            model_volume(nullptr), geometry_id(volume->geometry_id), volume_idx(-1) {}
        ModelVolumeState(const ModelVolume* model_volume, const ObjectID& instance_id, const GLVolume::CompositeID& composite_id) :
            model_volume(model_volume), geometry_id(std::make_pair(model_volume->id().id, instance_id.id)), composite_id(composite_id), volume_idx(-1) {}
        ModelVolumeState(const ObjectID& volume_id, const ObjectID& instance_id) :
            model_volume(nullptr), geometry_id(std::make_pair(volume_id.id, instance_id.id)), volume_idx(-1) {}
        bool new_geometry() const { return this->volume_idx == size_t(-1); }
        const ModelVolume* model_volume;
        // ObjectID of ModelVolume + ObjectID of ModelInstance
        // or timestamp of an SLAPrintObjectStep + ObjectID of ModelInstance
        std::pair<size_t, size_t>   geometry_id;
        GLVolume::CompositeID       composite_id;
        // Volume index in the new GLVolume vector.
        size_t                      volume_idx;
    };
    std::vector<ModelVolumeState> model_volume_state;
    std::vector<ModelVolumeState> aux_volume_state;

    struct GLVolumeState {
        GLVolumeState() :
            volume_idx(size_t(-1)) {}
        GLVolumeState(const GLVolume* volume, unsigned int volume_idx) :
            composite_id(volume->composite_id), volume_idx(volume_idx) {}
        GLVolumeState(const GLVolume::CompositeID &composite_id) :
            composite_id(composite_id), volume_idx(size_t(-1)) {}

        GLVolume::CompositeID       composite_id;
        // Volume index in the old GLVolume vector.
        size_t                      volume_idx;
    };

    // SLA steps to pull the preview meshes for.
	typedef std::array<SLAPrintObjectStep, 3> SLASteps;
    SLASteps sla_steps = { slaposDrillHoles, slaposSupportTree, slaposPad };
    struct SLASupportState {
        std::array<PrintStateBase::StateWithTimeStamp, std::tuple_size<SLASteps>::value> step;
    };
    // State of the sla_steps for all SLAPrintObjects.
    std::vector<SLASupportState>   sla_support_state;

    std::vector<size_t> instance_ids_selected;
    std::vector<size_t> map_glvolume_old_to_new(m_volumes.volumes.size(), size_t(-1));
    std::vector<GLVolumeState> deleted_volumes;
    // BBS
    std::vector<GLVolumeState> deleted_wipe_towers;
    std::vector<GLVolume*> glvolumes_new;
    glvolumes_new.reserve(m_volumes.volumes.size());
    auto model_volume_state_lower = [](const ModelVolumeState& m1, const ModelVolumeState& m2) { return m1.geometry_id < m2.geometry_id; };

    m_reload_delayed = !m_canvas->IsShown() && !refresh_immediately && !force_full_scene_refresh;

    PrinterTechnology printer_technology = current_printer_technology();

    // BBS: support wipe tower for multi-plates
    PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
    int n_plates = ppl.get_plate_count();
    std::vector<int> volume_idxs_wipe_tower_old(n_plates, -1);

    // Release invalidated volumes to conserve GPU memory in case of delayed refresh (see m_reload_delayed).
    // First initialize model_volumes_new_sorted & model_instances_new_sorted.
    for (int object_idx = 0; object_idx < (int)m_model->objects.size(); ++object_idx) {
        const ModelObject* model_object = m_model->objects[object_idx];
        for (int instance_idx = 0; instance_idx < (int)model_object->instances.size(); ++instance_idx) {
            const ModelInstance* model_instance = model_object->instances[instance_idx];
            for (int volume_idx = 0; volume_idx < (int)model_object->volumes.size(); ++volume_idx) {
                const ModelVolume* model_volume = model_object->volumes[volume_idx];
                if (m_canvas_type == ECanvasType::CanvasAssembleView) {
                    if (model_volume->is_model_part())
                        model_volume_state.emplace_back(model_volume, model_instance->id(), GLVolume::CompositeID(object_idx, volume_idx, instance_idx));
                }
                else {
                    model_volume_state.emplace_back(model_volume, model_instance->id(), GLVolume::CompositeID(object_idx, volume_idx, instance_idx));
                }
            }
        }
    }
    if (printer_technology == ptSLA) {
        const SLAPrint* sla_print = this->sla_print();
#ifndef NDEBUG
        // Verify that the SLAPrint object is synchronized with m_model.
        check_model_ids_equal(*m_model, sla_print->model());
#endif /* NDEBUG */
        sla_support_state.reserve(sla_print->objects().size());
        for (const SLAPrintObject* print_object : sla_print->objects()) {
            SLASupportState state;
            for (size_t istep = 0; istep < sla_steps.size(); ++istep) {
                state.step[istep] = print_object->step_state_with_timestamp(sla_steps[istep]);
                if (state.step[istep].state == PrintStateBase::DONE) {
                    if (!print_object->has_mesh(sla_steps[istep]))
                        // Consider the DONE step without a valid mesh as invalid for the purpose
                        // of mesh visualization.
                        state.step[istep].state = PrintStateBase::INVALID;
                    else if (sla_steps[istep] != slaposDrillHoles)
                        for (const ModelInstance* model_instance : print_object->model_object()->instances)
                            // Only the instances, which are currently printable, will have the SLA support structures kept.
                            // The instances outside the print bed will have the GLVolumes of their support structures released.
                            if (model_instance->is_printable())
                                aux_volume_state.emplace_back(state.step[istep].timestamp, model_instance->id());
                }
            }
            sla_support_state.emplace_back(state);
        }
    }
    std::sort(model_volume_state.begin(), model_volume_state.end(), model_volume_state_lower);
    std::sort(aux_volume_state.begin(), aux_volume_state.end(), model_volume_state_lower);

    // BBS: normalize painting data with current filament count
    for (unsigned int obj_idx = 0; obj_idx < (unsigned int)m_model->objects.size(); ++obj_idx) {
        const ModelObject& model_object = *m_model->objects[obj_idx];
        for (int volume_idx = 0; volume_idx < (int)model_object.volumes.size(); ++volume_idx) {
            ModelVolume& model_volume = *model_object.volumes[volume_idx];
            if (!model_volume.is_model_part())
                continue;

            unsigned int filaments_count = (unsigned int)dynamic_cast<const ConfigOptionStrings*>(m_config->option("filament_colour"))->values.size();
            model_volume.update_extruder_count(filaments_count);
        }
    }

    // Release all ModelVolume based GLVolumes not found in the current Model. Find the GLVolume of a hollowed mesh.
    for (size_t volume_id = 0; volume_id < m_volumes.volumes.size(); ++volume_id) {
        GLVolume* volume = m_volumes.volumes[volume_id];
        ModelVolumeState  key(volume);
        ModelVolumeState* mvs = nullptr;
        if (volume->volume_idx() < 0) {
            auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
            if (it != aux_volume_state.end() && it->geometry_id == key.geometry_id)
                // This can be an SLA support structure that should not be rendered (in case someone used undo
                // to revert to before it was generated). We only reuse the volume if that's not the case.
                if (m_model->objects[volume->composite_id.object_id]->sla_points_status != sla::PointsStatus::NoPoints)
                    mvs = &(*it);
        }
        else {
            auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
            if (it != model_volume_state.end() && it->geometry_id == key.geometry_id)
                mvs = &(*it);
        }
        // Emplace instance ID of the volume. Both the aux volumes and model volumes share the same instance ID.
        // The wipe tower has its own wipe_tower_instance_id().
        if (m_selection.contains_volume(volume_id)) {
            if (m_canvas_type == ECanvasType::CanvasAssembleView) {
                if (!volume->is_modifier)
                    instance_ids_selected.emplace_back(volume->geometry_id.second);
            }
            else {
                instance_ids_selected.emplace_back(volume->geometry_id.second);
            }
        }
        if (mvs == nullptr || force_full_scene_refresh) {
            // This GLVolume will be released.
            if (volume->is_wipe_tower) {
                // There is only one wipe tower.
                //assert(volume_idx_wipe_tower_old == -1);
                int plate_id = volume->composite_id.object_id - 1000;
                if (plate_id < n_plates)
                    volume_idxs_wipe_tower_old[plate_id] = (int)volume_id;
            }
            if (!m_reload_delayed) {
                deleted_volumes.emplace_back(volume, volume_id);
                // BBS
                if (volume->is_wipe_tower)
                    deleted_wipe_towers.emplace_back(volume, volume_id);
                delete volume;
            }
        }
        else {
            // This GLVolume will be reused.
            volume->set_sla_shift_z(0.0);
            map_glvolume_old_to_new[volume_id] = glvolumes_new.size();
            mvs->volume_idx = glvolumes_new.size();
            glvolumes_new.emplace_back(volume);
            // Update color of the volume based on the current extruder.
            if (mvs->model_volume != nullptr) {
                int extruder_id = mvs->model_volume->extruder_id();
                if (extruder_id != -1)
                    volume->extruder_id = extruder_id;

                volume->is_modifier = !mvs->model_volume->is_model_part();
                volume->set_color(color_from_model_volume(*mvs->model_volume));

                // updates volumes transformations
                if (m_canvas_type != ECanvasType::CanvasAssembleView) {
                    volume->set_instance_transformation(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_transformation());
                    volume->set_volume_transformation(mvs->model_volume->get_transformation());
                    // updates volumes convex hull
                    if (mvs->model_volume->is_model_part() && ! volume->convex_hull())
                        // Model volume was likely changed from modifier or support blocker / enforcer to a model part.
                        // Only model parts require convex hulls.
                        volume->set_convex_hull(mvs->model_volume->get_convex_hull_shared_ptr());
                    volume->set_offset_to_assembly(Vec3d(0, 0, 0));
                }
                else {
                    volume->set_instance_transformation(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_assemble_transformation());
                    volume->set_volume_transformation(mvs->model_volume->get_transformation());
                    // updates volumes convex hull
                    if (mvs->model_volume->is_model_part() && ! volume->convex_hull())
                        // Model volume was likely changed from modifier or support blocker / enforcer to a model part.
                        // Only model parts require convex hulls.
                        volume->set_convex_hull(mvs->model_volume->get_convex_hull_shared_ptr());
                    volume->set_offset_to_assembly(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_offset_to_assembly());
                }
            }
        }
    }
    sort_remove_duplicates(instance_ids_selected);
    auto deleted_volumes_lower = [](const GLVolumeState &v1, const GLVolumeState &v2) { return v1.composite_id < v2.composite_id; };
    std::sort(deleted_volumes.begin(), deleted_volumes.end(), deleted_volumes_lower);

    //BBS clean hover_volume_idxs
    m_hover_volume_idxs.clear();

    if (m_reload_delayed)
        return;

    // BBS: do not check wipe tower changes
    bool update_object_list = false;
    if (deleted_volumes.size() != deleted_wipe_towers.size())
        update_object_list = true;

    if (m_volumes.volumes != glvolumes_new && !update_object_list) {
        int vol_idx = 0;
        for (; vol_idx < std::min(m_volumes.volumes.size(), glvolumes_new.size()); vol_idx++) {
            if (m_volumes.volumes[vol_idx] != glvolumes_new[vol_idx]) {
                update_object_list = true;
                break;
            }
        }
        for (int temp_idx = vol_idx; temp_idx < m_volumes.volumes.size() && !update_object_list; temp_idx++) {
            // Volumes in m_volumes might not exist anymore, so we cannot
            // directly check if they are is_wipe_towers, for which we do
            // not want to update the object list.  Instead, we do a kind of
            // slow thing of seeing if they were in the deleted list, and if
            // so, if they were a wipe tower.
            bool was_deleted_wipe_tower = false;
            for (int del_idx = 0; del_idx < deleted_wipe_towers.size(); del_idx++) {
                if (deleted_wipe_towers[del_idx].volume_idx == temp_idx) {
                    was_deleted_wipe_tower = true;
                    break;
                }
            }
            if (!was_deleted_wipe_tower) {
                update_object_list = true;
            }
        }
        for (int temp_idx = vol_idx; temp_idx < glvolumes_new.size() && !update_object_list; temp_idx++) {
            if (!glvolumes_new[temp_idx]->is_wipe_tower)
                update_object_list = true;
        }
    }
    m_volumes.volumes = std::move(glvolumes_new);
    for (unsigned int obj_idx = 0; obj_idx < (unsigned int)m_model->objects.size(); ++ obj_idx) {
        const ModelObject &model_object = *m_model->objects[obj_idx];
        for (int volume_idx = 0; volume_idx < (int)model_object.volumes.size(); ++ volume_idx) {
			const ModelVolume &model_volume = *model_object.volumes[volume_idx];
            if (m_canvas_type == ECanvasType::CanvasAssembleView && !model_volume.is_model_part())
                continue;
            for (int instance_idx = 0; instance_idx < (int)model_object.instances.size(); ++ instance_idx) {
				const ModelInstance &model_instance = *model_object.instances[instance_idx];
				ModelVolumeState key(model_volume.id(), model_instance.id());
				auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
				assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                if (it->new_geometry()) {
                    // New volume.
                    auto it_old_volume = std::lower_bound(deleted_volumes.begin(), deleted_volumes.end(), GLVolumeState(it->composite_id), deleted_volumes_lower);
                    if (it_old_volume != deleted_volumes.end() && it_old_volume->composite_id == it->composite_id)
                        // If a volume changed its ObjectID, but it reuses a GLVolume's CompositeID, maintain its selection.
                        map_glvolume_old_to_new[it_old_volume->volume_idx] = m_volumes.volumes.size();
                    // Note the index of the loaded volume, so that we can reload the main model GLVolume with the hollowed mesh
                    // later in this function.
                    it->volume_idx = m_volumes.volumes.size();
                    m_volumes.load_object_volume(&model_object, obj_idx, volume_idx, instance_idx, m_color_by, m_initialized, m_canvas_type == ECanvasType::CanvasAssembleView);
                    m_volumes.volumes.back()->geometry_id = key.geometry_id;
                    update_object_list = true;
                } else {
					// Recycling an old GLVolume.
					GLVolume &existing_volume = *m_volumes.volumes[it->volume_idx];
                    assert(existing_volume.geometry_id == key.geometry_id);
					// Update the Object/Volume/Instance indices into the current Model.
					if (existing_volume.composite_id != it->composite_id) {
						existing_volume.composite_id = it->composite_id;
						update_object_list = true;
					}
                }
            }
        }
    }
    if (printer_technology == ptSLA) {
        size_t idx = 0;
        const SLAPrint *sla_print = this->sla_print();
		std::vector<double> shift_zs(m_model->objects.size(), 0);
        double relative_correction_z = sla_print->relative_correction().z();
        if (relative_correction_z <= EPSILON)
            relative_correction_z = 1.;
		for (const SLAPrintObject *print_object : sla_print->objects()) {
            SLASupportState   &state        = sla_support_state[idx ++];
            const ModelObject *model_object = print_object->model_object();
            // Find an index of the ModelObject
            int object_idx;
            // There may be new SLA volumes added to the scene for this print_object.
            // Find the object index of this print_object in the Model::objects list.
            auto it = std::find(sla_print->model().objects.begin(), sla_print->model().objects.end(), model_object);
            assert(it != sla_print->model().objects.end());
			object_idx = it - sla_print->model().objects.begin();
			// Cache the Z offset to be applied to all volumes with this object_idx.
			shift_zs[object_idx] = print_object->get_current_elevation() / relative_correction_z;
            // Collect indices of this print_object's instances, for which the SLA support meshes are to be added to the scene.
            // pairs of <instance_idx, print_instance_idx>
			std::vector<std::pair<size_t, size_t>> instances[std::tuple_size<SLASteps>::value];
            for (size_t print_instance_idx = 0; print_instance_idx < print_object->instances().size(); ++ print_instance_idx) {
                const SLAPrintObject::Instance &instance = print_object->instances()[print_instance_idx];
                // Find index of ModelInstance corresponding to this SLAPrintObject::Instance.
				auto it = std::find_if(model_object->instances.begin(), model_object->instances.end(),
                    [&instance](const ModelInstance *mi) { return mi->id() == instance.instance_id; });
                assert(it != model_object->instances.end());
                int instance_idx = it - model_object->instances.begin();
                for (size_t istep = 0; istep < sla_steps.size(); ++ istep)
                    if (sla_steps[istep] == slaposDrillHoles) {
                    	// Hollowing is a special case, where the mesh from the backend is being loaded into the 1st volume of an instance,
                    	// not into its own GLVolume.
                        // There shall always be such a GLVolume allocated.
                        ModelVolumeState key(model_object->volumes.front()->id(), instance.instance_id);
                        auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
                        assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                        assert(!it->new_geometry());
                        GLVolume &volume = *m_volumes.volumes[it->volume_idx];
                        if (! volume.offsets.empty() && state.step[istep].timestamp != volume.offsets.front()) {
                        	// The backend either produced a new hollowed mesh, or it invalidated the one that the front end has seen.
                            volume.model.reset();
                            if (state.step[istep].state == PrintStateBase::DONE) {
                                TriangleMesh mesh = print_object->get_mesh(slaposDrillHoles);
	                            assert(! mesh.empty());
                                mesh.transform(sla_print->sla_trafo(*m_model->objects[volume.object_idx()]).inverse());
#if ENABLE_SMOOTH_NORMALS
                                volume.model.init_from(mesh, true);
#else
                                volume.model.init_from(mesh);
                                volume.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<TriangleMesh>(mesh));
#endif // ENABLE_SMOOTH_NORMALS
                            }
                            else {
	                        	// Reload the original volume.
#if ENABLE_SMOOTH_NORMALS
                                volume.model.init_from(m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh(), true);
#else
                                const TriangleMesh& new_mesh = m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh();
                                volume.model.init_from(new_mesh);
                                volume.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<TriangleMesh>(new_mesh));
#endif // ENABLE_SMOOTH_NORMALS
                            }
	                    }
                    	//FIXME it is an ugly hack to write the timestamp into the "offsets" field to not have to add another member variable
                    	// to the GLVolume. We should refactor GLVolume significantly, so that the GLVolume will not contain member variables
                    	// of various concenrs (model vs. 3D print path).
                    	volume.offsets = { state.step[istep].timestamp };
                    }
                    else if (state.step[istep].state == PrintStateBase::DONE) {
                        // Check whether there is an existing auxiliary volume to be updated, or a new auxiliary volume to be created.
						ModelVolumeState key(state.step[istep].timestamp, instance.instance_id.id);
						auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
						assert(it != aux_volume_state.end() && it->geometry_id == key.geometry_id);
                    	if (it->new_geometry()) {
                            // This can be an SLA support structure that should not be rendered (in case someone used undo
                            // to revert to before it was generated). If that's the case, we should not generate anything.
                            if (model_object->sla_points_status != sla::PointsStatus::NoPoints)
                                instances[istep].emplace_back(std::pair<size_t, size_t>(instance_idx, print_instance_idx));
                            else
                                shift_zs[object_idx] = 0.;
                        }
                        else {
                            // Recycling an old GLVolume. Update the Object/Instance indices into the current Model.
                            m_volumes.volumes[it->volume_idx]->composite_id = GLVolume::CompositeID(object_idx, m_volumes.volumes[it->volume_idx]->volume_idx(), instance_idx);
                            m_volumes.volumes[it->volume_idx]->set_instance_transformation(model_object->instances[instance_idx]->get_transformation());
                        }
                    }
            }

            for (size_t istep = 0; istep < sla_steps.size(); ++istep)
                if (!instances[istep].empty())
                    m_volumes.load_object_auxiliary(print_object, object_idx, instances[istep], sla_steps[istep], state.step[istep].timestamp);
        }

		// Shift-up all volumes of the object so that it has the right elevation with respect to the print bed
		for (GLVolume* volume : m_volumes.volumes)
			if (volume->object_idx() < (int)m_model->objects.size() && m_model->objects[volume->object_idx()]->instances[volume->instance_idx()]->is_printable())
				volume->set_sla_shift_z(shift_zs[volume->object_idx()]);
    }

    // BBS
    if (printer_technology == ptFFF && m_config->has("filament_colour") && (m_canvas_type != ECanvasType::CanvasAssembleView)) {
        // Should the wipe tower be visualized ?
        unsigned int filaments_count = (unsigned int)dynamic_cast<const ConfigOptionStrings*>(m_config->option("filament_colour"))->values.size();

        bool wt = dynamic_cast<const ConfigOptionBool*>(m_config->option("enable_prime_tower"))->value;
        auto co = dynamic_cast<const ConfigOptionEnum<PrintSequence>*>(m_config->option<ConfigOptionEnum<PrintSequence>>("print_sequence"));

        const DynamicPrintConfig &dconfig           = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        auto timelapse_type = dconfig.option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
        bool timelapse_enabled = timelapse_type ? (timelapse_type->value == TimelapseType::tlSmooth) : false;

        if (wt && (timelapse_enabled || filaments_count > 1)) {
            for (int plate_id = 0; plate_id < n_plates; plate_id++) {
                // If print ByObject and there is only one object in the plate, the wipe tower is allowed to be generated.
                PartPlate* part_plate = ppl.get_plate(plate_id);
                if (part_plate->get_print_seq() == PrintSequence::ByObject ||
                    (part_plate->get_print_seq() == PrintSequence::ByDefault && co != nullptr && co->value == PrintSequence::ByObject)) {
                    if (ppl.get_plate(plate_id)->printable_instance_size() != 1)
                        continue;
                }

                DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
                float x = dynamic_cast<const ConfigOptionFloats*>(proj_cfg.option("wipe_tower_x"))->get_at(plate_id);
                float y = dynamic_cast<const ConfigOptionFloats*>(proj_cfg.option("wipe_tower_y"))->get_at(plate_id);
                float w = dynamic_cast<const ConfigOptionFloat*>(m_config->option("prime_tower_width"))->value;
                float a = dynamic_cast<const ConfigOptionFloat*>(proj_cfg.option("wipe_tower_rotation_angle"))->value;
                float tower_brim_width = dynamic_cast<const ConfigOptionFloat*>(m_config->option("prime_tower_brim_width"))->value;
                // BBS
                // float v = dynamic_cast<const ConfigOptionFloat*>(m_config->option("prime_volume"))->value;
                Vec3d plate_origin = ppl.get_plate(plate_id)->get_origin();

                const Print* print = m_process->fff_print();
                const auto& wipe_tower_data = print->wipe_tower_data(filaments_count);
                float brim_width = wipe_tower_data.brim_width;
                const DynamicPrintConfig &print_cfg   = wxGetApp().preset_bundle->prints.get_edited_preset().config;
                Vec3d wipe_tower_size = ppl.get_plate(plate_id)->estimate_wipe_tower_size(print_cfg, w, wipe_tower_data.depth);

                const float   margin     = WIPE_TOWER_MARGIN + tower_brim_width;
                BoundingBoxf3 plate_bbox = wxGetApp().plater()->get_partplate_list().get_plate(plate_id)->get_bounding_box();
                coordf_t plate_bbox_x_max_local_coord = plate_bbox.max(0) - plate_origin(0);
                coordf_t plate_bbox_y_max_local_coord = plate_bbox.max(1) - plate_origin(1);
                bool need_update = false;
                if (x + margin + wipe_tower_size(0) > plate_bbox_x_max_local_coord) {
                    x = plate_bbox_x_max_local_coord - wipe_tower_size(0) - margin;
                    need_update = true;
                }
                else if (x < margin) {
                    x = margin;
                    need_update = true;
                }
                if (need_update) {
                    ConfigOptionFloat wt_x_opt(x);
                    dynamic_cast<ConfigOptionFloats *>(proj_cfg.option("wipe_tower_x"))->set_at(&wt_x_opt, plate_id, 0);
                    need_update = false;
                }

                if (y + margin + wipe_tower_size(1) > plate_bbox_y_max_local_coord) {
                    y = plate_bbox_y_max_local_coord - wipe_tower_size(1) - margin;
                    need_update = true;
                }
                else if (y < margin) {
                    y = margin;
                    need_update = true;
                }
                if (need_update) {
                    ConfigOptionFloat wt_y_opt(y);
                    dynamic_cast<ConfigOptionFloats *>(proj_cfg.option("wipe_tower_y"))->set_at(&wt_y_opt, plate_id, 0);
                }

                int volume_idx_wipe_tower_new = m_volumes.load_wipe_tower_preview(
                    1000 + plate_id, x + plate_origin(0), y + plate_origin(1),
                    (float)wipe_tower_size(0), (float)wipe_tower_size(1), (float)wipe_tower_size(2), a,
                    /*!print->is_step_done(psWipeTower)*/ true, brim_width);
                int volume_idx_wipe_tower_old = volume_idxs_wipe_tower_old[plate_id];
                if (volume_idx_wipe_tower_old != -1)
                    map_glvolume_old_to_new[volume_idx_wipe_tower_old] = volume_idx_wipe_tower_new;
            }
        }
    }

    update_volumes_colors_by_extruder();
	// Update selection indices based on the old/new GLVolumeCollection.
    if (m_selection.get_mode() == Selection::Instance)
        m_selection.instances_changed(instance_ids_selected);
    else
        m_selection.volumes_changed(map_glvolume_old_to_new);

    // @Enrico suggest this solution to preven accessing pointer on caster without data
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::Bed);
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::Volume);
    m_gizmos.update_data();
    m_gizmos.update_assemble_view_data();
    m_gizmos.refresh_on_off_state();

    // Update the toolbar
    //BBS: notify the PartPlateList to reload all objects
    if (update_object_list)
        post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));

    //BBS:exclude the assmble view
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        _set_warning_notification_if_needed(EWarning::GCodeConflict);
        // checks for geometry outside the print volume to render it accordingly
        if (!m_volumes.empty()) {
            ModelInstanceEPrintVolumeState state;
            const bool contained_min_one = m_volumes.check_outside_state(m_bed.build_volume(), &state);
            const bool partlyOut = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Partly_Outside);
            const bool fullyOut = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Fully_Outside);

            _set_warning_notification(EWarning::ObjectClashed, partlyOut);
            //BBS: turn off the warning when fully outside
            //_set_warning_notification(EWarning::ObjectOutside, fullyOut);
            if (printer_technology != ptSLA || !contained_min_one)
                _set_warning_notification(EWarning::SlaSupportsOutside, false);

            post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS,
                contained_min_one && !m_model->objects.empty() && !partlyOut));
        }
        else {
            _set_warning_notification(EWarning::ObjectOutside, false);
            _set_warning_notification(EWarning::ObjectClashed, false);
            _set_warning_notification(EWarning::SlaSupportsOutside, false);
            post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
        }
    }

    refresh_camera_scene_box();

    if (m_selection.is_empty()) {
        // If no object is selected, deactivate the active gizmo, if any
        // Otherwise it may be shown after cleaning the scene (if it was active while the objects were deleted)
        m_gizmos.reset_all_states();
        // BBS
#if 0
        // If no object is selected, reset the objects manipulator on the sidebar
        // to force a reset of its cache
        auto manip = wxGetApp().obj_manipul();
        if (manip != nullptr)
            manip->set_dirty();
#endif
    }

    // refresh bed raycasters for picking
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        wxGetApp().plater()->get_partplate_list().register_raycasters_for_picking(*this);
    }

    // refresh volume raycasters for picking
    for (size_t i = 0; i < m_volumes.volumes.size(); ++i) {
        const GLVolume* v = m_volumes.volumes[i];
        assert(v->mesh_raycaster != nullptr);
        std::shared_ptr<SceneRaycasterItem> raycaster = add_raycaster_for_picking(SceneRaycaster::EType::Volume, i, *v->mesh_raycaster, v->world_matrix());
        raycaster->set_active(v->is_active);
    }

    // refresh gizmo elements raycasters for picking
    GLGizmoBase* curr_gizmo = m_gizmos.get_current();
    if (curr_gizmo != nullptr)
        curr_gizmo->unregister_raycasters_for_picking();
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::Gizmo);
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::FallbackGizmo);
    if (curr_gizmo != nullptr && !m_selection.is_empty())
        curr_gizmo->register_raycasters_for_picking();

    // and force this canvas to be redrawn.
    m_dirty = true;
}

void GLCanvas3D::load_shells(const Print& print, bool force_previewing)
{
    if (m_initialized)
    {
        m_gcode_viewer.load_shells(print, m_initialized, force_previewing);
        m_gcode_viewer.update_shells_color_by_extruder(m_config);
    }
}

void GLCanvas3D::set_shell_transparence(float alpha){
    m_gcode_viewer.set_shell_transparency(alpha);

}
//BBS: add only gcode mode
void GLCanvas3D::load_gcode_preview(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors, bool only_gcode)
{
    PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
    PartPlate* plate = partplate_list.get_curr_plate();
    const std::vector<BoundingBoxf3>& exclude_bounding_box = plate->get_exclude_areas();

    //BBS: init is called in GLCanvas3D.render()
    //when load gcode directly, it is too late
    m_gcode_viewer.init(wxGetApp().get_mode(), wxGetApp().preset_bundle);
    m_gcode_viewer.load(gcode_result, *this->fff_print(), wxGetApp().plater()->build_volume(), exclude_bounding_box,
        wxGetApp().get_mode(), only_gcode);
    m_gcode_viewer.get_moves_slider()->SetHigherValue(m_gcode_viewer.get_moves_slider()->GetMaxValue());

    if (wxGetApp().is_editor()) {
        //BBS: always load shell at preview, do this in load_shells
        //m_gcode_viewer.update_shells_color_by_extruder(m_config);
        _set_warning_notification_if_needed(EWarning::ToolHeightOutside);
        _set_warning_notification_if_needed(EWarning::ToolpathOutside);
        _set_warning_notification_if_needed(EWarning::GCodeConflict);
    }

    m_gcode_viewer.refresh(gcode_result, str_tool_colors);
    set_as_dirty();
    request_extra_frame();
}

void GLCanvas3D::refresh_gcode_preview_render_paths()
{
    m_gcode_viewer.refresh_render_paths();
    set_as_dirty();
    request_extra_frame();
}

void GLCanvas3D::load_sla_preview()
{
    const SLAPrint* print = sla_print();
    if (m_canvas != nullptr && print != nullptr) {
        _set_current();
	    // Release OpenGL data before generating new data.
	    reset_volumes();
        _load_sla_shells();
        _update_sla_shells_outside_state();
        _set_warning_notification_if_needed(EWarning::SlaSupportsOutside);
    }
}

/*void GLCanvas3D::load_preview(const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values)
{
    const Print *print = this->fff_print();
    if (print == nullptr)
        return;

    _set_current();

    // Release OpenGL data before generating new data.
    this->reset_volumes();

    const BuildVolume &build_volume = m_bed.build_volume();
    _load_print_toolpaths(build_volume);
    _load_wipe_tower_toolpaths(build_volume, str_tool_colors);
    for (const PrintObject* object : print->objects())
        _load_print_object_toolpaths(*object, build_volume, str_tool_colors, color_print_values);

    _set_warning_notification_if_needed(EWarning::ToolpathOutside);
}*/

void GLCanvas3D::bind_event_handlers()
{
    if (m_canvas != nullptr) {
        m_canvas->Bind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Bind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Bind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Bind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Bind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Bind(EVT_GLCANVAS_RENDER_TIMER, &GLCanvas3D::on_render_timer, this);
        m_toolbar_highlighter.set_timer_owner(m_canvas, 0);
        m_canvas->Bind(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, [this](wxTimerEvent&) { m_toolbar_highlighter.blink(); });
        m_gizmo_highlighter.set_timer_owner(m_canvas, 0);
        m_canvas->Bind(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, [this](wxTimerEvent&) { m_gizmo_highlighter.blink(); });
        m_canvas->Bind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
        m_canvas->Bind(wxEVT_SET_FOCUS, &GLCanvas3D::on_set_focus, this);
        m_canvas->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& evt) {
                ImGui::SetWindowFocus(nullptr);
                render();
                evt.Skip();
            });
        m_event_handlers_bound = true;

        m_canvas->Bind(wxEVT_GESTURE_PAN, &GLCanvas3D::on_gesture, this);
        m_canvas->Bind(wxEVT_GESTURE_ZOOM, &GLCanvas3D::on_gesture, this);
        m_canvas->Bind(wxEVT_GESTURE_ROTATE, &GLCanvas3D::on_gesture, this);
        m_canvas->EnableTouchEvents(wxTOUCH_ZOOM_GESTURE | wxTOUCH_ROTATE_GESTURE);
#if __WXOSX__
        initGestures(m_canvas->GetHandle(), m_canvas); // for UIPanGestureRecognizer allowedScrollTypesMask
#endif
    }
}

void GLCanvas3D::unbind_event_handlers()
{
    if (m_canvas != nullptr && m_event_handlers_bound) {
        m_canvas->Unbind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Unbind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Unbind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Unbind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Unbind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Unbind(EVT_GLCANVAS_RENDER_TIMER, &GLCanvas3D::on_render_timer, this);
        m_canvas->Unbind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
		m_canvas->Unbind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
        m_canvas->Unbind(wxEVT_SET_FOCUS, &GLCanvas3D::on_set_focus, this);
        m_event_handlers_bound = false;

        m_canvas->Unbind(wxEVT_GESTURE_PAN, &GLCanvas3D::on_gesture, this);
        m_canvas->Unbind(wxEVT_GESTURE_ZOOM, &GLCanvas3D::on_gesture, this);
        m_canvas->Unbind(wxEVT_GESTURE_ROTATE, &GLCanvas3D::on_gesture, this);
    }
}

void GLCanvas3D::on_size(wxSizeEvent& evt)
{
    m_dirty = true;
}

void GLCanvas3D::on_idle(wxIdleEvent& evt)
{
    if (!m_initialized)
        return;

    m_dirty |= m_main_toolbar.update_items_state();
    //BBS: GUI refactor: GLToolbar
    m_dirty |= m_assemble_view_toolbar.update_items_state();
    // BBS
    //m_dirty |= wxGetApp().plater()->get_view_toolbar().update_items_state();
    m_dirty |= wxGetApp().plater()->get_collapse_toolbar().update_items_state();
    _update_imgui_select_plate_toolbar();
    bool mouse3d_controller_applied = wxGetApp().plater()->get_mouse3d_controller().apply(wxGetApp().plater()->get_camera());
    m_dirty |= mouse3d_controller_applied;
    m_dirty |= wxGetApp().plater()->get_notification_manager()->update_notifications(*this);
    auto gizmo = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager().get_current();
    if (gizmo != nullptr) m_dirty |= gizmo->update_items_state();
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    // ImGuiWrapper::m_requires_extra_frame may have been set by a render made outside of the OnIdle mechanism
    bool imgui_requires_extra_frame = wxGetApp().imgui()->requires_extra_frame();
    m_dirty |= imgui_requires_extra_frame;
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    if (!m_dirty)
        return;

#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    // this needs to be done here.
    // during the render launched by the refresh the value may be set again
    wxGetApp().imgui()->reset_requires_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    _refresh_if_shown_on_screen();

#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    if (m_extra_frame_requested || mouse3d_controller_applied || imgui_requires_extra_frame || wxGetApp().imgui()->requires_extra_frame()) {
#else
    if (m_extra_frame_requested || mouse3d_controller_applied) {
        m_dirty = true;
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
        m_extra_frame_requested = false;
        evt.RequestMore();
    }
    else
        m_dirty = false;
}

void GLCanvas3D::on_char(wxKeyEvent& evt)
{
    if (!m_initialized)
        return;

    // see include/wx/defs.h enum wxKeyCode
    int keyCode = evt.GetKeyCode();
    int ctrlMask = wxMOD_CONTROL;
    int shiftMask = wxMOD_SHIFT;

    auto imgui = wxGetApp().imgui();
    if (imgui->update_key_data(evt)) {
        render();
        return;
    }

    bool is_in_painting_mode = false;
    GLGizmoPainterBase *current_gizmo_painter = dynamic_cast<GLGizmoPainterBase *>(get_gizmos_manager().get_current());
    if (current_gizmo_painter != nullptr) {
        is_in_painting_mode = true;
    }

    //BBS: add orient deactivate logic
    if (keyCode == WXK_ESCAPE
        && (_deactivate_arrange_menu() || _deactivate_orient_menu()))
        return;

    if (m_gizmos.on_char(evt))
        return;

    if ((evt.GetModifiers() & ctrlMask) != 0) {
        // CTRL is pressed
        switch (keyCode) {
#ifdef __APPLE__
        case 'a':
        case 'A':
#else /* __APPLE__ */
        case WXK_CONTROL_A:
#endif /* __APPLE__ */
            if (!is_in_painting_mode && !m_layers_editing.is_enabled())
                post_event(SimpleEvent(EVT_GLCANVAS_SELECT_ALL));
        break;
#ifdef __APPLE__
        case 'c':
        case 'C':
#else /* __APPLE__ */
        case WXK_CONTROL_C:
#endif /* __APPLE__ */
            if (!is_in_painting_mode)
                post_event(SimpleEvent(EVT_GLTOOLBAR_COPY));
        break;
#ifdef __APPLE__
        case 'm':
        case 'M':
#else /* __APPLE__ */
        case WXK_CONTROL_M:
#endif /* __APPLE__ */
        {
#ifde
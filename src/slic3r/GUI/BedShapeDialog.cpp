#include "BedShapeDialog.hpp"
#include "GUI_App.hpp"
#include "OptionsGroup.hpp"

#include <wx/wx.h>
#include <wx/numformatter.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/tooltip.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"

#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/DialogButtons.hpp"

#include <algorithm>

namespace Slic3r {
namespace GUI {

BedShape::BedShape(const Pointfs& points)
{
    m_build_volume = { points, 0. };
}

static std::string get_option_label(BedShape::Parameter param)
{
    switch (param) {
    case BedShape::Parameter::RectSize  : return L("Size");
    case BedShape::Parameter::RectOrigin: return L("Origin");
    default:                              assert(false); return {};
    }
}

void BedShape::append_option_line(ConfigOptionsGroupShp optgroup, Parameter param)
{
    ConfigOptionDef def;
    t_config_option_key key;
    switch (param) {
    case Parameter::RectSize:
        def.type = coPoints;
        def.set_default_value(new ConfigOptionPoints{ Vec2d(200, 200) });
        def.min = 0;
        def.max = 214700;
        def.width = 10; // increase width for large scale printers with 4 digit values
        def.sidetext = "mm";	// milimeters, don't need translation
        def.label = get_option_label(param);
        def.tooltip = L("Size in X and Y of the rectangular plate.");
        key = "rect_size";
        break;
    case Parameter::RectOrigin:
        def.type = coPoints;
        def.set_default_value(new ConfigOptionPoints{ Vec2d(0, 0) });
        def.min = -107350;
        def.max = 107350;
        def.width = 10; // increase width for large scale printers with 4 digit values
        def.sidetext = "mm";	// milimeters, don't need translation
        def.label = get_option_label(param);
        def.tooltip = L("Distance of the 0,0 G-code coordinate from the front left corner of the rectangle.");
        key = "rect_origin";
        break;
    default:
        assert(false);
    }

    optgroup->append_single_option_line({ def, std::move(key) });
}

wxString BedShape::get_name(PageType type)
{
    return _L("Rectangular");
}

BedShape::PageType BedShape::get_page_type()
{
    return PageType::Rectangle;
}

wxString BedShape::get_full_name_with_params()
{
    wxString out = _L("Shape") + ": " + get_name(this->get_page_type());
    out += "\n" + _(get_option_label(Parameter::RectSize)) + ": [" + ConfigOptionPoint(to_2d(m_build_volume.bounding_volume().size())).serialize() + "]";
    out += "\n" + _(get_option_label(Parameter::RectOrigin)) + ": [" + ConfigOptionPoint(- to_2d(m_build_volume.bounding_volume().min)).serialize() + "]";
    return out;
}

void BedShape::apply_optgroup_values(ConfigOptionsGroupShp optgroup)
{
    optgroup->set_value("rect_size"  , new ConfigOptionPoints{ to_2d(m_build_volume.bounding_volume().size()) });
    optgroup->set_value("rect_origin", new ConfigOptionPoints{ - to_2d(m_build_volume.bounding_volume().min) });
}

void BedShapeDialog::build_dialog(const Pointfs& default_pt, const ConfigOptionString& custom_texture, const ConfigOptionString& custom_model)
{
    SetFont(wxGetApp().normal_font());

    SetBackgroundColour(*wxWHITE);
	m_panel = new BedShapePanel(this);
    m_panel->build_panel(default_pt, custom_texture, custom_model);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(m_panel, 1, wxEXPAND);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    main_sizer->Add(dlg_btns, 0, wxEXPAND);


	SetSizer(main_sizer);
	SetMinSize(GetSize());
	main_sizer->SetSizeHints(this);

    this->Bind(wxEVT_CLOSE_WINDOW, ([this](wxCloseEvent& evt) {
        EndModal(wxID_CANCEL);
    }));
}

void BedShapeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    const int& em = em_unit();

    for (auto og : m_panel->m_optgroups)
        og->msw_rescale();

    const wxSize& size = wxSize(64 * em, -1);

    SetMinSize(size);
    SetSize(size);

    Refresh();
}

const std::string BedShapePanel::EMPTY_STRING = "";

void BedShapePanel::build_panel(const Pointfs& default_pt, const std::string& custom_texture, const std::string& custom_model)
{
    m_shape = make_counter_clockwise(default_pt);

    // ORCA match style of wxStaticBox between platforms
    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Shape"));
    auto sbsizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    auto optgroup = init_shape_options_page(BedShape::get_name(BedShape::PageType::Rectangle));
    BedShape::append_option_line(optgroup, BedShape::Parameter::RectSize);
    BedShape::append_option_line(optgroup, BedShape::Parameter::RectOrigin);
    activate_options_page(optgroup);

    sbsizer->Add(optgroup->parent(), 1, wxEXPAND);

	// right pane with preview canvas
	m_canvas = new Bed_2D(this);
    m_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent& e) { m_canvas->repaint(m_shape); });
    m_canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { m_canvas->Refresh(); });

    wxSizer* left_sizer = new wxBoxSizer(wxVERTICAL);
    left_sizer->Add(sbsizer, 1, wxEXPAND);

    wxSizer* top_sizer = new wxBoxSizer(wxHORIZONTAL);
    top_sizer->Add(left_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 10);
    top_sizer->Add(m_canvas, 1, wxEXPAND | wxALL, 10);

	SetSizerAndFit(top_sizer);

	set_shape(m_shape);
	update_preview();
}

ConfigOptionsGroupShp BedShapePanel::init_shape_options_page(const wxString& title)
{
    wxPanel* panel = new wxPanel(this);
    panel->SetBackgroundColour(*wxWHITE);
    ConfigOptionsGroupShp optgroup = std::make_shared<ConfigOptionsGroup>(panel, _L("Settings"));

    optgroup->label_width = 10;
    optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        update_shape();
    };

    m_optgroups.push_back(optgroup);

    return optgroup;
}

void BedShapePanel::activate_options_page(ConfigOptionsGroupShp options_group)
{
    options_group->activate();
    options_group->parent()->SetSizerAndFit(options_group->sizer);
}

void BedShapePanel::set_shape(const Pointfs& points)
{
    BedShape shape(points);
    shape.apply_optgroup_values(m_optgroups[0]);
    update_shape();
}

void BedShapePanel::update_preview()
{
	if (m_canvas) m_canvas->Refresh();
	Refresh();
}

void BedShapePanel::update_shape()
{
    auto opt_group = m_optgroups[0];

    Vec2d rect_size(Vec2d::Zero());
    Vec2d rect_origin(Vec2d::Zero());

    try { rect_size = boost::any_cast<Vec2d>(opt_group->get_value("rect_size")); }
    catch (const std::exception& /* e */) { return; }

    try { rect_origin = boost::any_cast<Vec2d>(opt_group->get_value("rect_origin")); }
    catch (const std::exception& /* e */) { return; }

    auto x = rect_size(0);
    auto y = rect_size(1);
    if (x == 0 || y == 0) return;

    double x0 = 0.0 - rect_origin(0);
    double y0 = 0.0 - rect_origin(1);
    double x1 = x - rect_origin(0);
    double y1 = y - rect_origin(1);

    m_shape = { Vec2d(x0, y0), Vec2d(x1, y0), Vec2d(x1, y1), Vec2d(x0, y1) };

    update_preview();
}

} // GUI
} // Slic3r

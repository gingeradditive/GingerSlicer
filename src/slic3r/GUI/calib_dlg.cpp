#include "calib_dlg.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include <wx/dcgraph.h>
#include "MainFrame.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Plater.hpp"
#include <string>

namespace Slic3r { namespace GUI {

namespace {

int GetTextMax(wxWindow* parent, const std::vector<wxString>& labels)
{
    wxSize text_size;
    for (wxString label : labels)
        text_size.IncTo(parent->GetTextExtent(label));
    return text_size.x + parent->FromDIP(10);
}

}

// Param_Sweep_Dlg
//

// Parameters that can be swept layer by layer. Units are shown in the label.
// ERS parameters are applied by the PressureEqualizer (require 'Extrusion rate
// smoothing' > 0), the retraction ones by the G-code writer at each layer change.
struct SweepParamEntry {
    const char *key;       // config key stored in Calib_Params::sweep_param
    const char *label;     // human readable label with units
    double      def_start;
    double      def_end;
    double      def_step;
};
static const SweepParamEntry s_sweep_params[] = {
    {"",                                     "-",                                             0,   0,  0},
    {"retraction_length",                    "Retraction length (mm)",                        0,   2,  0.05},
    {"retraction_speed",                     "Retraction speed (mm/s)",                       10,  60, 1},
    {"deretraction_speed",                   "Deretraction speed (mm/s)",                     10,  60, 1},
    {"retract_restart_extra",                "Extra length on restart (mm)",                  0,   1,  0.02},
    {"max_volumetric_extrusion_rate_slope",  "ERS smoothing slope (mm3/s2)",                  5,   40, 0.5},
    {"pellet_ers_deceleration_slope",        "ERS deceleration slope (mm3/s2)",               5,   40, 0.5},
    {"pellet_ers_min_rate",                  "ERS minimum flow rate (mm3/s)",                 0.2, 3,  0.05},
    {"pellet_ers_ramp_profile",              "ERS ramp profile (0=linear 1=sqrt 2=exp)",      0,   2,  1},
    {"pellet_ers_rampup_flow",               "ERS ramp-up flow (%)",                          100, 160, 1},
    {"pellet_ers_rampdown_flow",             "ERS ramp-down flow (%)",                        100, 40,  1},
    {"pellet_ers_pressure_tau",              "ERS pressure time constant (s)",                0,   0.5, 0.005},
};

Param_Sweep_Dlg::Param_Sweep_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Parameter tuning (per-layer sweep)"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 320)), wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE);
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);

    wxString param_str = _L("Parameter: ");
    wxString start_str = _L("Start value: ");
    wxString end_str   = _L("End value: ");
    wxString step_str  = _L("Step per layer: ");
    int text_max = GetTextMax(this, std::vector<wxString>{param_str, start_str, end_str, step_str});

    auto st_size = FromDIP(wxSize(text_max, -1));
    auto ti_size = FromDIP(wxSize(220, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // parameter selector
    auto param_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto param_text = new wxStaticText(this, wxID_ANY, param_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_cbParam = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, ti_size, 0, nullptr, wxCB_READONLY);
    m_cbParam->Append(_L("Disabled (remove sweep)"));
    for (size_t i = 1; i < sizeof(s_sweep_params) / sizeof(s_sweep_params[0]); ++i)
        m_cbParam->Append(wxString::FromUTF8(s_sweep_params[i].label));
    m_cbParam->SetSelection(1);
    param_sizer->Add(param_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    param_sizer->Add(m_cbParam , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(param_sizer, 0, wxLEFT, FromDIP(3));

    // start value
    auto start_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_text = new wxStaticText(this, wxID_ANY, start_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStart = new TextInput(this, wxString::FromDouble(s_sweep_params[1].def_start), "", "", wxDefaultPosition, ti_size);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    start_sizer->Add(start_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    start_sizer->Add(m_tiStart , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(start_sizer, 0, wxLEFT, FromDIP(3));

    // end value
    auto end_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_text = new wxStaticText(this, wxID_ANY, end_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, wxString::FromDouble(s_sweep_params[1].def_end), "", "", wxDefaultPosition, ti_size);
    m_tiEnd->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_sizer->Add(end_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    end_sizer->Add(m_tiEnd , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(end_sizer, 0, wxLEFT, FromDIP(3));

    // step per layer
    auto step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto step_text = new wxStaticText(this, wxID_ANY, step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(s_sweep_params[1].def_step), "", "", wxDefaultPosition, ti_size);
    m_tiStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    step_sizer->Add(step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    step_sizer->Add(m_tiStep , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(step_sizer, 0, wxLEFT, FromDIP(3));

    settings_sizer->AddSpacer(FromDIP(5));

    auto note_text = new wxStaticText(this, wxID_ANY,
        _L("The sweep is applied to the objects currently on the plate: the value starts at\n"
           "'Start', changes by 'Step' at every layer and holds once 'End' is reached.\n"
           "The active value is written in the G-code as a comment at each layer.\n"
           "ERS parameters require 'Extrusion rate smoothing' > 0.\n"
           "Re-slice after pressing OK."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    note_text->SetForegroundColour(wxColour(128, 128, 128));
    settings_sizer->Add(note_text, 0, wxALL, FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    auto dlg_btns = new DialogButtons(this, {"OK"});
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Param_Sweep_Dlg::on_start, this);
    m_cbParam->Bind(wxEVT_COMBOBOX, &Param_Sweep_Dlg::on_param_changed, this);

    Layout();
    Fit();
}

Param_Sweep_Dlg::~Param_Sweep_Dlg() {
    // Disconnect Events
}

void Param_Sweep_Dlg::on_param_changed(wxCommandEvent& event) {
    const int sel = m_cbParam->GetSelection();
    if (sel <= 0)
        return;
    const SweepParamEntry &entry = s_sweep_params[sel];
    m_tiStart->GetTextCtrl()->SetValue(wxString::FromDouble(entry.def_start));
    m_tiEnd->GetTextCtrl()->SetValue(wxString::FromDouble(entry.def_end));
    m_tiStep->GetTextCtrl()->SetValue(wxString::FromDouble(entry.def_step));
}

void Param_Sweep_Dlg::on_start(wxCommandEvent& event) {
    const int sel = m_cbParam->GetSelection();
    if (sel <= 0) {
        // Remove any active sweep.
        m_params = Calib_Params();
        m_params.mode = CalibMode::Calib_None;
        m_plater->calib_param_sweep(m_params);
        EndModal(wxID_OK);
        return;
    }

    bool read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    if (!read_double || m_params.step <= 0 || m_params.start == m_params.end) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstep > 0\nstart != end"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    // Refuse configurations where the sweep would silently have no effect.
    {
        const std::string key = s_sweep_params[sel].key;
        const bool is_ers_param = key == "max_volumetric_extrusion_rate_slope" || key == "pellet_ers_deceleration_slope" ||
                                  key == "pellet_ers_min_rate" || key == "pellet_ers_ramp_profile" ||
                                  key == "pellet_ers_rampup_flow" || key == "pellet_ers_rampdown_flow" ||
                                  key == "pellet_ers_pressure_tau";
        if (is_ers_param) {
            const DynamicPrintConfig &print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            wxString warn;
            if (print_cfg.option<ConfigOptionFloat>("max_volumetric_extrusion_rate_slope")->value <= 0)
                warn = _L("'Extrusion rate smoothing' is 0: the ERS post-processor is disabled and this sweep would have no effect.\nSet a smoothing slope > 0 first.");
            else if (key != "max_volumetric_extrusion_rate_slope" && !print_cfg.opt_bool("pellet_ers_mode"))
                warn = _L("'Pellet extruder mode' is disabled: this parameter only affects Pellet ERS mode and the sweep would have no effect.\nEnable pellet ERS mode first.");
            if (!warn.empty()) {
                MessageDialog msg_dlg(nullptr, warn, wxEmptyString, wxICON_WARNING | wxOK);
                msg_dlg.ShowModal();
                return;
            }
        }
    }

    m_params.sweep_param = s_sweep_params[sel].key;
    m_params.mode = CalibMode::Calib_Param_Sweep;
    m_plater->calib_param_sweep(m_params);
    EndModal(wxID_OK);
}

void Param_Sweep_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();
}

}} // namespace Slic3r::GUI

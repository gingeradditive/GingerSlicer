#include "calib_dlg.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include <wx/dcgraph.h>
#include "MainFrame.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
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
// For per-object sweeps both are switched at every object change inside the layer
// (GCode::apply_per_object_sweep).
// The step per layer is normally left empty (automatic: the sweep spans the target's
// whole layer range, see calib_sweep_effective_step), so no default step is proposed.
struct SweepParamEntry {
    const char *key;       // config key stored in Calib_Params::sweep_param
    const char *label;     // human readable label with units
    double      def_start;
    double      def_end;
};
static const SweepParamEntry s_sweep_params[] = {
    {"",                                     "-",                                             0,   0},
    {"retraction_length",                    "Retraction length (mm)",                        0,   2},
    {"retraction_speed",                     "Retraction speed (mm/s)",                       10,  60},
    {"deretraction_speed",                   "Deretraction speed (mm/s)",                     10,  60},
    {"retract_restart_extra",                "Extra length on restart (mm)",                  0,   1},
    {"max_volumetric_extrusion_rate_slope",  "ERS smoothing slope (mm3/s2)",                  5,   40},
    {"pellet_ers_deceleration_slope",        "ERS deceleration slope (mm3/s2)",               5,   40},
    {"pellet_ers_min_rate",                  "ERS minimum flow rate (mm3/s)",                 0.2, 3},
    {"pellet_ers_ramp_profile",              "ERS ramp profile (0=linear 1=sqrt 2=exp)",      0,   2},
    {"pellet_ers_rampup_flow",               "ERS ramp-up flow (%)",                          100, 160},
    {"pellet_ers_rampdown_flow",             "ERS ramp-down flow (%)",                        100, 40},
    {"pellet_ers_pressure_tau",              "ERS pressure time constant (s)",                0,   0.5},
};

Param_Sweep_Dlg::Param_Sweep_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Parameter tuning (per-layer sweep)"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 320)), wxDEFAULT_DIALOG_STYLE), m_plater(plater)
{
    SetBackgroundColour(*wxWHITE);
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);

    wxString target_str = _L("Apply to: ");
    wxString param_str = _L("Parameter: ");
    wxString start_str = _L("Start value: ");
    wxString end_str   = _L("End value: ");
    wxString step_str  = _L("Step per layer: ");
    int text_max = GetTextMax(this, std::vector<wxString>{target_str, param_str, start_str, end_str, step_str});

    auto st_size = FromDIP(wxSize(text_max, -1));
    auto ti_size = FromDIP(wxSize(220, -1));

    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Settings"));
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);

    settings_sizer->AddSpacer(FromDIP(5));

    // target selector: all objects (single global sweep) or one object, so several
    // per-object sweeps can be combined in one print
    auto target_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto target_text = new wxStaticText(this, wxID_ANY, target_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_cbTarget = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, ti_size, 0, nullptr, wxCB_READONLY);
    target_sizer->Add(target_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    target_sizer->Add(m_cbTarget , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(target_sizer, 0, wxLEFT, FromDIP(3));

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

    // step per layer (empty = automatic)
    auto step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto step_text = new wxStaticText(this, wxID_ANY, step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxEmptyString, "", "", wxDefaultPosition, ti_size);
    m_tiStep->GetTextCtrl()->SetHint(_L("auto"));
    m_tiStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    step_sizer->Add(step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    step_sizer->Add(m_tiStep , 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    settings_sizer->Add(step_sizer, 0, wxLEFT, FromDIP(3));

    settings_sizer->AddSpacer(FromDIP(5));

    auto note_text = new wxStaticText(this, wxID_ANY,
        _L("With 'Step' empty (auto) the value goes from 'Start' at the first layer to\n"
           "'End' exactly at the target's last layer. With an explicit step it changes\n"
           "by 'Step' at every layer and holds once 'End' is reached. The active value\n"
           "is written in the G-code as a comment.\n"
           "With 'All objects' one single sweep is applied to the whole plate; targeting\n"
           "an object lets you combine several sweeps (one per object) in one print.\n"
           "Setting a sweep replaces the previous one for the same target; 'Disabled'\n"
           "removes the target's sweep. ERS parameters require 'Extrusion rate\n"
           "smoothing' > 0. 'Apply' sets the sweep and keeps the window open,\n"
           "'OK' sets it and closes. Re-slice when done."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    note_text->SetForegroundColour(wxColour(128, 128, 128));
    settings_sizer->Add(note_text, 0, wxALL, FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    // currently active sweeps on the plate's print
    LabeledStaticBox* active_stb = new LabeledStaticBox(this, _L("Active sweeps"));
    wxStaticBoxSizer* active_sizer = new wxStaticBoxSizer(active_stb, wxVERTICAL);
    m_active_text = new wxStaticText(this, wxID_ANY, _L("none"), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    active_sizer->Add(m_active_text, 0, wxALL, FromDIP(5));
    v_sizer->Add(active_sizer, 0, wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));

    update_targets_and_summary();

    auto dlg_btns = new DialogButtons(this, {"Apply", "OK"});
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    dlg_btns->GetAPPLY()->Bind(wxEVT_BUTTON, &Param_Sweep_Dlg::on_apply, this);
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Param_Sweep_Dlg::on_ok, this);
    m_cbParam->Bind(wxEVT_COMBOBOX, &Param_Sweep_Dlg::on_param_changed, this);
    m_cbTarget->Bind(wxEVT_COMBOBOX, &Param_Sweep_Dlg::on_target_changed, this);

    Layout();
    Fit();
}

Param_Sweep_Dlg::~Param_Sweep_Dlg() {
    // Disconnect Events
}

int Param_Sweep_Dlg::ShowModal() {
    // The dialog is created once and reused: refresh the object list and the summary
    // of the sweeps currently set on the print.
    update_targets_and_summary();
    Layout();
    Fit();
    return DPIDialog::ShowModal();
}

void Param_Sweep_Dlg::update_targets_and_summary() {
    // Keep the current target selected across refreshes (e.g. after Apply), so several
    // values can be tuned on the same object without re-picking it every time.
    const int prev_sel = m_cbTarget->GetSelection();
    const int prev_id  = (prev_sel >= 0 && prev_sel < int(m_target_ids.size())) ? m_target_ids[prev_sel] : -1;
    m_cbTarget->Clear();
    m_target_ids.clear();
    m_cbTarget->Append(_L("All objects (single sweep)"));
    m_target_ids.push_back(-1);
    for (const ModelObject *mo : m_plater->model().objects) {
        m_cbTarget->Append(wxString::FromUTF8(mo->name));
        m_target_ids.push_back(int(mo->id().id));
    }
    int sel = 0;
    for (size_t i = 0; i < m_target_ids.size(); ++i)
        if (m_target_ids[i] == prev_id) {
            sel = int(i);
            break;
        }
    m_cbTarget->SetSelection(sel);

    // NOT Plater::fff_print(): that is the legacy single print, while the sweeps are
    // set by Plater::calib_param_sweep on the current plate's own Print.
    const Print &print = m_plater->get_partplate_list().get_current_fff_print();
    wxString summary;
    for (const Calib_Params &p : print.calib_params()) {
        wxString target = _L("all objects");
        if (p.object_id >= 0) {
            // The object may have been deleted since the sweep was set.
            target = wxString::Format(_L("object #%d (removed)"), p.object_id);
            for (const ModelObject *mo : m_plater->model().objects)
                if (int(mo->id().id) == p.object_id) {
                    target = wxString::FromUTF8(mo->name);
                    break;
                }
        }
        wxString label = wxString::FromUTF8(p.sweep_param);
        for (size_t i = 1; i < sizeof(s_sweep_params) / sizeof(s_sweep_params[0]); ++i)
            if (p.sweep_param == s_sweep_params[i].key) {
                label = wxString::FromUTF8(s_sweep_params[i].label);
                break;
            }
        if (!summary.empty())
            summary += "\n";
        const wxString step_txt = p.step > 0 ? wxString::Format("%g", p.step) : _L("auto");
        summary += wxString::Format("%s - %s: %g -> %g, step %s", target, label, p.start, p.end, step_txt);
    }
    if (summary.empty())
        summary = _L("none");
    m_active_text->SetLabel(summary);

    // Reflect the (possibly) active sweep of the initial target in the fields.
    load_sweep_for_target();
}

void Param_Sweep_Dlg::on_param_changed(wxCommandEvent& event) {
    const int sel = m_cbParam->GetSelection();
    if (sel <= 0)
        return;
    const SweepParamEntry &entry = s_sweep_params[sel];
    m_tiStart->GetTextCtrl()->SetValue(wxString::FromDouble(entry.def_start));
    m_tiEnd->GetTextCtrl()->SetValue(wxString::FromDouble(entry.def_end));
    // A step tuned for another parameter makes no sense here: back to automatic.
    m_tiStep->GetTextCtrl()->SetValue(wxEmptyString);
}

void Param_Sweep_Dlg::on_target_changed(wxCommandEvent& event) {
    load_sweep_for_target();
}

void Param_Sweep_Dlg::load_sweep_for_target() {
    const int target_sel = m_cbTarget->GetSelection();
    if (target_sel < 0 || target_sel >= int(m_target_ids.size()))
        return;
    const Calib_Params *cp = m_plater->get_partplate_list().get_current_fff_print().calib_params_for_object(m_target_ids[target_sel]);
    if (cp == nullptr)
        // No sweep on this target yet: keep the current fields as a template, so the
        // same range can be quickly applied to several objects.
        return;
    for (size_t i = 1; i < sizeof(s_sweep_params) / sizeof(s_sweep_params[0]); ++i)
        if (cp->sweep_param == s_sweep_params[i].key) {
            m_cbParam->SetSelection(int(i));
            break;
        }
    m_tiStart->GetTextCtrl()->SetValue(wxString::FromDouble(cp->start));
    m_tiEnd->GetTextCtrl()->SetValue(wxString::FromDouble(cp->end));
    m_tiStep->GetTextCtrl()->SetValue(cp->step > 0 ? wxString::FromDouble(cp->step) : wxString());
}

void Param_Sweep_Dlg::on_apply(wxCommandEvent& event) {
    if (apply_sweep())
        // Show the result right away and keep going: the typical flow is setting
        // several per-object sweeps in a row.
        update_targets_and_summary();
}

void Param_Sweep_Dlg::on_ok(wxCommandEvent& event) {
    if (apply_sweep())
        EndModal(wxID_OK);
}

bool Param_Sweep_Dlg::apply_sweep() {
    const int target_sel = m_cbTarget->GetSelection();
    const int target_id  = (target_sel >= 0 && target_sel < int(m_target_ids.size())) ? m_target_ids[target_sel] : -1;
    const int sel = m_cbParam->GetSelection();
    if (sel <= 0) {
        // Remove the target's sweep ("All objects" removes every sweep).
        m_params = Calib_Params();
        m_params.mode = CalibMode::Calib_None;
        m_params.object_id = target_id;
        m_plater->calib_param_sweep(m_params);
        return true;
    }

    bool read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);

    // Empty step = automatic (resolved at G-code time from the target's layer count);
    // an explicit step must be positive.
    wxString step_str = m_tiStep->GetTextCtrl()->GetValue();
    step_str.Trim(true).Trim(false);
    m_params.step = 0.;
    const bool step_ok = step_str.IsEmpty() || (step_str.ToDouble(&m_params.step) && m_params.step > 0);

    if (!read_double || !step_ok || m_params.start == m_params.end) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstep empty (auto) or > 0\nstart != end"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return false;
    }

    // Refuse configurations where the sweep would silently have no effect.
    {
        const std::string key = s_sweep_params[sel].key;
        if (calib_is_ers_param(key)) {
            const DynamicPrintConfig &print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            wxString warn;
            if (print_cfg.option<ConfigOptionFloat>("max_volumetric_extrusion_rate_slope")->value <= 0)
                warn = _L("'Extrusion rate smoothing' is 0: the ERS post-processor is disabled and this sweep would have no effect.\nSet a smoothing slope > 0 first.");
            else if (key != "max_volumetric_extrusion_rate_slope" && !print_cfg.opt_bool("pellet_ers_mode"))
                warn = _L("'Pellet extruder mode' is disabled: this parameter only affects Pellet ERS mode and the sweep would have no effect.\nEnable pellet ERS mode first.");
            if (!warn.empty()) {
                MessageDialog msg_dlg(nullptr, warn, wxEmptyString, wxICON_WARNING | wxOK);
                msg_dlg.ShowModal();
                return false;
            }
        }
    }

    m_params.sweep_param = s_sweep_params[sel].key;
    m_params.mode = CalibMode::Calib_Param_Sweep;
    m_params.object_id = target_id;
    m_plater->calib_param_sweep(m_params);
    return true;
}

void Param_Sweep_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();
}

}} // namespace Slic3r::GUI

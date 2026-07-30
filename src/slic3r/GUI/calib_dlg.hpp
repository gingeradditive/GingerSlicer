#ifndef slic3r_calib_dlg_hpp_
#define slic3r_calib_dlg_hpp_

#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/RadioGroup.hpp"
#include "GUI_App.hpp"
#include "wx/hyperlink.h"
#include <wx/radiobox.h>
#include "libslic3r/calib.hpp"

namespace Slic3r { namespace GUI {

class Param_Sweep_Dlg : public DPIDialog
{
public:
    Param_Sweep_Dlg(wxWindow* parent, wxWindowID id, Plater* plater);
    ~Param_Sweep_Dlg();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    // Refreshes the target-object list and the active-sweeps summary on every open:
    // the dialog instance is created once and reused by MainFrame.
    int ShowModal() override;

protected:
    // Apply keeps the dialog open (typical flow: set several per-object sweeps in a
    // row), OK applies and closes. Both share apply_sweep().
    virtual void on_apply(wxCommandEvent& event);
    virtual void on_ok(wxCommandEvent& event);
    virtual void on_param_changed(wxCommandEvent& event);
    virtual void on_target_changed(wxCommandEvent& event);
    // Validate the fields and set/remove the sweep on the print; false = invalid input.
    bool apply_sweep();
    // Repopulate m_cbTarget from the model and m_active_text from the print's sweeps,
    // keeping the current target selected when it still exists.
    void update_targets_and_summary();
    // If the selected target already has a sweep, load it into the fields so pressing
    // OK re-saves it instead of overwriting it with the previous target's values.
    void load_sweep_for_target();
    Calib_Params m_params;

    ComboBox* m_cbParam;
    // Sweep target: "All objects" or a single object of the model, so several
    // per-object sweeps can be combined in one print.
    ComboBox* m_cbTarget;
    // ModelObject ObjectIDs parallel to m_cbTarget entries; -1 = all objects.
    std::vector<int> m_target_ids;
    wxStaticText* m_active_text;
    TextInput* m_tiStart;
    TextInput* m_tiEnd;
    TextInput* m_tiStep;
    Plater* m_plater;
};
}} // namespace Slic3r::GUI
#endif
